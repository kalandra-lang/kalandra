#include "../gpu-source/ClosureNativeLayout.h"
#include "../gpu-source/ClosureGraphSchema.h"
#include "../gpu-source/PointerBridge.h"
#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
int main(int argc, char **argv) {
  try {
    if (argc != 2 && argc != 3)
      throw std::runtime_error("expected LLVM output path");
    auto checked = check(parse(std::make_shared<SourceFile>("closure-bridge.k", R"k(
fn named(value:int)->int:
  value+1
fn make(offset:int,choice:int)->fn(int)->int:
  captured=fn(value:int):value+offset
  if choice==0:named
  else:captured
fn main()->int:
  callback=make(9,1)
  callback(1)
)k")));
    if (!checked.ok()) {
      for (const auto &diagnostic : checked.diagnostics)
        std::cerr << renderDiagnostic(*checked.source, diagnostic);
      throw std::runtime_error("closure bridge source rejected");
    }
    const auto &source = *checked.module;
    FunctionId factory = source.functions.size();
    for (FunctionId id = 0; id < source.functions.size(); ++id)
      if (source.functions[id].name == "make")
        factory = id;
    if (factory == source.functions.size())
      throw std::runtime_error("missing source factory");
    const auto root = source.functions[factory].result;
    const auto plan = planClosureTransfer(source, std::span(&root, 1));
    if (!plan || plan->environments.size() != 2)
      throw std::runtime_error("source closure target set was not preserved");
    auto projected = continuation::plan(source, {.preserveCalls = true, .valueScopes = true});
    if (!projected)
      throw std::runtime_error(projected.error());
    const bool useProjection = argc == 3 && std::string_view(argv[2]) == "continuation";
    const auto &loweredSource = useProjection ? projected->kernels : source;
    const auto identity = [&](FunctionId id) {
      return useProjection ? projected->importedFunctions.at(id) : id;
    };
    const auto exportedFactory = identity(factory);
    std::vector<std::string> exports(loweredSource.functions.size());
    exports[exportedFactory] = loweredSource.functions.at(exportedFactory).name;
    const auto output = lowerNative(loweredSource, {}, exports);
    if (!output)
      throw std::runtime_error(output.error());
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(output->llvm, diagnostic, context);
    if (!module)
      throw std::runtime_error("source LLVM parse failed");
    const auto native = planNativeClosureLayouts(source, *plan, context, module->getDataLayout());
    if (!native)
      throw std::runtime_error(native.error());
    const auto records = planNativeAggregateLayouts(source, context, module->getDataLayout());
    if (!records)
      throw std::runtime_error(records.error());
    const auto graph = planClosureGraphSchema(source, std::span(&source.functions[factory].resultType, 1),
                                               *plan, *native, *records);
    if (!graph)
      throw std::runtime_error(graph.error());
    if (graph->graph.layouts.size() != 2 || graph->imports.size() != 2 ||
        graph->graph.layouts[0].cells.size() != 2 ||
        graph->graph.layouts[1].cells.size() != 1)
      throw std::runtime_error("source closure canonical schema disagrees with its capture ABI");
    auto *entry = module->getFunction("_mlir_ciface_kalandra_cpu_" + std::to_string(exportedFactory));
    if (!entry)
      throw std::runtime_error("source factory export is missing");
    emitCpuPointerBridge(*entry, "fixture_factory");
    std::vector<CpuClosureTarget> targets;
    for (const auto &environment : native->environments) {
      auto *code = module->getFunction("kalandra_callable_" + std::to_string(identity(environment.target)));
      if (!code)
        throw std::runtime_error("source callable identity is missing");
      const bool captures = !environment.captures.empty();
      targets.push_back({code, environment.target + 1, captures});
      const auto constant = [&](const char *name, std::uint64_t value) {
        auto *function = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {}, false),
            llvm::GlobalValue::ExternalLinkage, name, *module);
        llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", function));
        builder.CreateRet(builder.getInt64(value));
      };
      constant(captures ? "fixture_captured_tag" : "fixture_named_tag", environment.target + 1);
      if (captures) {
        if (environment.captures.size() != 1)
          throw std::runtime_error("unexpected source capture count");
        const auto &wireEnvironment = graph->graph.layouts[1];
        if (graph->environments[1] != environment.target)
          throw std::runtime_error("source graph environment target changed");
        constant("fixture_environment_bytes", wireEnvironment.nativeStride);
        constant("fixture_environment_alignment", wireEnvironment.nativeAlignment);
        constant("fixture_offset_field", wireEnvironment.cells[0].nativeOffset);
      }
    }
    emitCpuClosureBridge(*module, "fixture_identify", targets);
    if (llvm::verifyModule(*module, &llvm::errs()))
      throw std::runtime_error("source closure bridge LLVM is invalid");
    std::error_code error;
    llvm::raw_fd_ostream stream(argv[1], error);
    if (error)
      throw std::runtime_error(error.message());
    module->print(stream, nullptr);
    stream.flush();
    if (stream.has_error())
      throw std::runtime_error("could not write source closure bridge LLVM");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
