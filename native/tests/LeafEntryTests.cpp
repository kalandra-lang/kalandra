#include "../lib/LeafEntry.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Support/SourceMgr.h"
#include <iostream>

int main() {
  using namespace llvm;
  for (unsigned kind = 0; kind != 10; ++kind) {
    std::string prefix;
    if (kind == 1) prefix = "store volatile i64 %n, ptr %ctx\n";
    if (kind == 2) prefix = "%seen = load atomic i64, ptr %ctx acquire, align 8\n";
    if (kind == 3) prefix = "call void @foreign(ptr %ctx)\n";
    if (kind == 6) prefix = "store i64 %n, ptr %ctx\n";
    if (kind == 4) prefix = "%dynamic = alloca i8, i64 %n\n store volatile i8 0, ptr %dynamic\n";
    auto ir = std::string(R"(
      target triple = "arm64-apple-macosx26.0.0"
      target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
      declare void @foreign(ptr)
      define i64 @entry(i64 %n, ptr %ctx) #0 {
      start:
    )") + prefix + R"(
        %packet = alloca i64
        store i64 %n, ptr %packet
        %positive = icmp sgt i64 %n, 0
        br i1 %positive, label %value, label %managed
      value:
        %loaded = load i64, ptr %packet
        %result = add i64 %loaded, 7
        ret i64 %result
      managed:
        call void @foreign(ptr %ctx)
        ret i64 0
      }
      attributes #0 = { "kalandra.unbound.host-entry" "frame-pointer"="all" }
    )";
    LLVMContext context; SMDiagnostic diagnostics;
    auto module = parseAssemblyString(ir, diagnostics, context);
    if (!module) { diagnostics.print("entry-frames", errs()); return 1; }
    auto simplify = [](llvm::Function &fn, bool) {
      LoopAnalysisManager loops; FunctionAnalysisManager functions;
      CGSCCAnalysisManager calls; ModuleAnalysisManager modules; PassBuilder passes;
      passes.registerLoopAnalyses(loops); passes.registerFunctionAnalyses(functions);
      passes.registerCGSCCAnalyses(calls); passes.registerModuleAnalyses(modules);
      passes.crossRegisterProxies(loops, functions, calls, modules);
      auto pipeline = passes.buildFunctionSimplificationPipeline(OptimizationLevel::O2,
                                                                  ThinOrFullLTOPhase::None);
      pipeline.run(fn, functions);
    };
    if (kind == 5) module->getFunction("entry")->addFnAttr("kalandra.boundary");
    if (kind == 7) module->getFunction("entry")->setSection("__TEXT,__kalandra,regular,pure_instructions");
    if (kind == 8) module->getFunction("entry")->addRetAttr(Attribute::ZExt);
    if (kind == 9) module->getFunction("entry")->addParamAttr(0, Attribute::InReg);
    const bool changed = kalandra::outlineCheckedEntryFrames(*module, simplify);
    if (changed != (kind == 0 || kind >= 8) || verifyModule(*module, &errs())) {
      errs() << "leaf-entry case " << kind << " changed=" << changed << '\n'; return 1;
    }
    if (changed && kalandra::outlineCheckedEntryFrames(*module, simplify)) return 1;
    if (changed) {
      auto *fn = module->getFunction("entry");
      for (auto &block : *fn) for (auto &i : block) {
        if (isa<AllocaInst>(i) || isa<StoreInst>(i)) return 1;
        if (auto *call = dyn_cast<CallInst>(&i)) {
          if (!call->isMustTailCall()) return 1;
          if (kind == 8 && !call->hasRetAttr(Attribute::ZExt)) return 1;
          if (kind == 9 && !call->paramHasAttr(0, Attribute::InReg)) return 1;
        }
      }
      auto *worker = module->getFunction("entry.framed");
      if (!worker || worker->getFnAttribute("frame-pointer").getValueAsString() != "all" ||
          fn->getFnAttribute("frame-pointer").getValueAsString() != "none") return 1;
    }
  }
  std::cout << "10 leaf-entry frame, external-effect, token, dynamic-stack, protected-code and tail ABI cases passed\n";
}
