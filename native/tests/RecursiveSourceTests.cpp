#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
namespace kalandra {
// Private component path: source worker admission stays gated.
std::expected<BackendOutput, std::string>
lowerTaskNative(const TypedModule &, std::optional<std::string>,
                ArithmeticMode = ArithmeticMode::Hardware, const CpuTarget & = {});
} // namespace kalandra
using namespace kalandra;
static const std::string prefix = "enum List:\n  Nil\n  Cons:\n    head:i64\n    tail:List\n";
static CheckResult analyze(std::string source) {
  return check(parse(std::make_shared<SourceFile>("recursive-source.k", std::move(source))));
}
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  try {
#ifndef KALANDRA_RECURSIVE_SOURCE_FRONTEND_ONLY
    if (argc == 3) {
      std::ifstream input(argv[1]);
      require(bool(input), "cannot read continuation fixture");
      auto checked = analyze(std::string(std::istreambuf_iterator<char>(input), {}));
      if (!checked.ok()) {
        for (const auto &d : checked.diagnostics)
          std::cerr << renderDiagnostic(*checked.source, d);
        return 1;
      }
      auto lowered = lowerTaskNative(*checked.module, "main");
      if (!lowered)
        throw std::runtime_error(lowered.error());
      std::ofstream output(argv[2]);
      output << lowered->llvm;
      require(bool(output), "cannot write continuation LLVM fixture");
      return 0;
    }
#endif
    require(argc == 1, "expected no arguments or source/output LLVM paths");
    (void)argv;
    auto valid = analyze(prefix + "fn sum(items:List)->i64:\n  return match items:\n    List.Nil:0\n"
                                  "    List.Cons {head,tail}:head+sum(tail)\nfn main()->i64:\n"
                                  "  return sum(List.Cons {head:42,tail:List.Nil})\n");
    if (!valid.ok())
      for (const auto &d : valid.diagnostics)
        std::cerr << renderDiagnostic(*valid.source, d);
    require(valid.ok() && !numericCapabilityError(*valid.module, NumericTarget::Cpu),
            "valid recursive source rejected");
    auto planned = continuation::plan(*valid.module);
    require(bool(planned), "checked recursive continuation plan rejected");
    auto preserved = continuation::plan(*valid.module, {.preserveCalls = true});
    require(preserved &&
                std::ranges::any_of(preserved->plans,
                                    [](const auto &plan) {
                                      return std::ranges::any_of(plan->code, [](const auto &op) {
                                        return op.code == continuation::Code::Call;
                                      });
                                    }),
            "recursive native helper was expanded instead of preserving its checked call");
    require(
        std::ranges::any_of(planned->plans,
                            [](const auto &plan) {
                              return std::ranges::any_of(plan->code, [](const auto &instruction) {
                                return instruction.code == continuation::Code::SynchronousCall;
                              });
                            }),
        "recursive call lost its checked synchronous boundary");
    require(bool(numericCapabilityError(*valid.module, NumericTarget::AppleGpu)),
            "recursive GPU admitted");
#ifndef KALANDRA_RECURSIVE_SOURCE_FRONTEND_ONLY
    auto lowered = lowerNative(*valid.module);
    require(lowered && lowered->mlir.find("scf.index_switch") != std::string::npos &&
                lowered->llvm.find("kalandra_recursive_make_v1") != std::string::npos,
            "recursive source did not use ordinary checked switch/boxing lowering");
    auto exported = lowerNative(*valid.module, {}, {"sum"});
    require(exported && exported->llvm.find("_mlir_ciface_kalandra_cpu_0") != std::string::npos &&
                exported->llvm.find("kalandra_recursive_read_v1") != std::string::npos,
            "recursive native bridge lost its checked nominal reads");
#endif
    auto text = analyze(
        "enum Text:\n  Empty\n  More:\n    text:String\n    next:Text\nfn main()->i64:\n  return 0\n");
    require(text.ok(), "recursive String schema rejected");
    auto collections =
        analyze("enum Chain:\n  End\n  Link:\n    values:[i64]\n"
                "    flags:[bool]\n    tensor:Tensor[f64,?,2]\n    names:[String]\n"
                "    next:Chain\nfn main()->i64:\n  return 0\n");
    require(collections.ok() && !numericCapabilityError(*collections.module, NumericTarget::Cpu),
            "recursive numeric/Bool collection schema rejected");
    auto tree = analyze(
        "type Tree:\n  children:[Tree]\nfn main()->i64:\n"
        "  leaf=Tree {children:[]}\n  root=Tree {children:[leaf,leaf]}\n  return length(root.children)\n");
    require(tree.ok(), "array-mediated recursive source rejected");
#ifndef KALANDRA_RECURSIVE_SOURCE_FRONTEND_ONLY
    require(bool(lowerNative(*tree.module)), "array-mediated source failed native lowering");
#endif
    auto callable = analyze(
        "enum Chain:\n  End\n  More:\n    apply:fn(i64)->i64\n    next:Chain\n"
        "fn make(offset:i64)->Chain:\n  return Chain.More {apply:fn(value):return value+offset,next:Chain.End}\n"
        "fn main()->i64:\n  chain=make(20)\n  return match chain:\n    Chain.End:0\n    Chain.More "
        "{apply}:apply(22)\n");
    require(callable.ok(), "recursive captured callable source rejected");
#ifndef KALANDRA_RECURSIVE_SOURCE_FRONTEND_ONLY
    auto callableIr = lowerNative(*callable.module);
    require(callableIr && callableIr->llvm.find("kalandra_recursive_environment_read_v1") !=
                              std::string::npos,
            "recursive callable did not use checked managed environment reads");
#endif
    auto functionArray =
        analyze("enum Chain:\n  End\n  More:\n    callbacks:[fn(i64)->i64]\n    next:Chain\n"
                "fn make(offset:i64)->Chain:\n  add=fn(value:i64):return value+offset\n"
                "  return Chain.More {callbacks:[add,add],next:Chain.End}\n"
                "fn main()->i64:\n  return match make(20):\n    Chain.End:0\n"
                "    Chain.More {callbacks}:callbacks[0](1)+callbacks[1](1)\n");
    require(functionArray.ok(), "recursive function array source rejected");
#ifndef KALANDRA_RECURSIVE_SOURCE_FRONTEND_ONLY
    auto arrayIr = lowerNative(*functionArray.module);
    require(arrayIr && arrayIr->llvm.find("kalandra_recursive_function_array_dependency_v1") !=
                           std::string::npos,
            "recursive function arrays did not use checked ownership bridge");
#endif
    for (const auto &source : {"enum Loop:\n  More:\n    next:Loop\nfn main()->i64:\n  return 0\n",
                               "type A:\n  child:B\ntype B:\n  child:A\nfn main()->i64:\n  return 0\n"}) {
      auto failed = analyze(source);
      require(!failed.ok() && !failed.diagnostics.empty(), "infinite recursive schema admitted");
    }
    for (const auto &source : {"enum Text:\n  Empty\n  More:\n    text:[fn(i64)->i64 ! "
                               "TaskError]\n    next:Text\nfn main()->i64:\n  return 0\n",
                               "enum Callback:\n  Empty\n  More:\n    call:fn(i64)->i64 ! "
                               "TaskError\n    next:Callback\nfn main()->i64:\n  return 0\n"}) {
      auto checked = analyze(source);
      require(checked.ok() && bool(continuation::plan(*checked.module)),
              "recursive fallible callable schema rejected");
    }
    auto worker =
        analyze(prefix + "fn echo(items:List)->List:\n  return items\nfn main()->i64 ! TaskError:\n  return "
                         "item=try parallel:\n    try await spawn echo(List.Nil)\n  return match item:\n  return "
                         "  List.Nil:42\n    List.Cons {head}:head\n");
    require(worker.ok() && bool(continuation::plan(*worker.module)),
            "recursive source worker rejected");
    for (const auto &tail :
         {"fn main()->i64:\n  return match List.Nil:\n    List.Cons {head}:head\n",
          "fn main()->i64:\n  return match List.Nil:\n    List.Nil:0\n    List.Nil:1\n    List.Cons:2\n"})
      require(!analyze(prefix + tail).ok(), "invalid recursive match admitted");
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
      auto forged = *valid.module;
      auto &expression = forged.expressions.at(forged.functions.back().result);
      if (mutation == 0)
        forged.records[0].fields[1].type.task = true;
      else if (mutation == 1)
        forged.records[0].fields[1].type.fallible = true;
      else if (mutation == 2)
        forged.records[0].errorMembers = {SIZE_MAX, SIZE_MAX};
      else
        expression.type.device = true;
      require(!continuation::plan(forged),
              "invalid recursive field/error/device IR reached planner");
    }
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
      auto forged = *valid.module;
      auto found = std::ranges::find_if(forged.expressions, [](const auto &expression) {
        return std::holds_alternative<MatchOperation>(expression.operation);
      });
      require(found != forged.expressions.end(), "recursive match IR was not preserved");
      auto &match = std::get<MatchOperation>(found->operation);
      switch (mutation) {
      case 0:
        match.subject = SIZE_MAX;
        break;
      case 1:
        match.cases.clear();
        break;
      case 2:
        match.cases[0].tag = 0;
        break;
      case 3:
        match.cases[1].tag = match.cases[0].tag;
        break;
      case 4:
        match.cases[0].tag.reset();
        break;
      case 5:
        match.cases[0].result = SIZE_MAX;
        break;
      case 6:
        match.cases.pop_back();
        break;
      case 7:
        match.cases[0].body.push_back({{}, {}, SIZE_MAX});
        break;
      }
      require(!continuation::plan(forged), "forged recursive match reached planner");
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)),
              "forged recursive match admitted");
#ifndef KALANDRA_RECURSIVE_SOURCE_FRONTEND_ONLY
      require(!lowerNative(forged), "forged recursive match reached backend");
#endif
    }
    std::cout << "recursive source frontend passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
