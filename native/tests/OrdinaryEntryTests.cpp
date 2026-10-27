#include "../lib/OrdinaryBaseCase.h"
#include "../lib/OrdinaryEntry.h"
#include "../lib/OrdinaryLoops.h"
#include "../lib/OrdinaryMain.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Support/SourceMgr.h"
#include <iostream>

static void simplify(llvm::Function &fn, bool) {
  using namespace llvm;
  LoopAnalysisManager loops;
  FunctionAnalysisManager functions;
  CGSCCAnalysisManager calls;
  ModuleAnalysisManager modules;
  PassBuilder passes;
  passes.registerLoopAnalyses(loops);
  passes.registerFunctionAnalyses(functions);
  passes.registerCGSCCAnalyses(calls);
  passes.registerModuleAnalyses(modules);
  passes.crossRegisterProxies(loops, functions, calls, modules);
  auto pipeline =
      passes.buildFunctionSimplificationPipeline(OptimizationLevel::O2, ThinOrFullLTOPhase::None);
  pipeline.run(fn, functions);
}

int main() {
  using namespace llvm;
  for (unsigned kind = 0; kind < 10; ++kind) {
    std::string body = "ret i64 %n";
    if (kind == 1)
      body = "call void @kalandra_fault_v1(ptr %ctx, i64 0, i64 12, i64 7)\n ret i64 0";
    if (kind == 2)
      body = "call void @foreign(ptr %ctx)\n ret i64 %n";
    if (kind == 3)
      body = "store volatile i64 %n, ptr @state\n ret i64 %n";
    if (kind == 4)
      body = "%seen = load atomic i64, ptr @state acquire, align 8\n ret i64 %seen";
    if (kind == 5)
      body = "%memory = call ptr @malloc(i64 64)\n %address = ptrtoint ptr %memory to i64\n ret "
             "i64 %address";
    if (kind == 6)
      body = "call void @kalandra_fault_v1(ptr %ctx, i64 4, i64 12, i64 7)\n call void "
             "@kalandra_fault_v1(ptr %ctx, i64 7, i64 99, i64 88)\n ret i64 0";
    if (kind == 9)
      body = "call void @kalandra_fault_v1(ptr %ctx, i64 4, i64 12, i64 7)\n ret i64 0";
    auto ir = std::string(R"(
      target triple = "arm64-apple-macosx26.0.0"
      target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
      @state = global i64 0
      declare void @kalandra_fault_v1(ptr,i64,i64,i64)
      declare void @foreign(ptr)
      declare ptr @malloc(i64)
      define i64 @_mlir_ciface_kalandra_test_entry(i64 %n, ptr %ctx) #0 {
    )") + body +
              R"(
      }
      attributes #0 = { "kalandra.unbound.host-entry" "frame-pointer"="all" }
    )";
    if (kind == 9)
      ir.replace(ir.find("define i64"), 10, "define range(i64 0, 100) i64");
    LLVMContext context;
    SMDiagnostic diagnostic;
    auto module = parseAssemblyString(ir, diagnostic, context);
    if (!module) {
      diagnostic.print("ordinary-entry", errs());
      return 1;
    }
    auto *original = module->getFunction("_mlir_ciface_kalandra_test_entry");
    if (kind == 7)
      original->addFnAttr("kalandra.task.frame");
    if (kind == 8)
      original->addRetAttr(Attribute::ZExt);
    const bool changed = kalandra::addOrdinaryScalarEntries(*module, simplify);
    if (changed != (kind < 2 || kind == 6 || kind == 8 || kind == 9) ||
        verifyModule(*module, &errs())) {
      errs() << "ordinary-entry case " << kind << " changed=" << changed << '\n';
      return 1;
    }
    if (changed) {
      if (kalandra::addOrdinaryScalarEntries(*module, simplify))
        return 1;
      auto *fn = module->getFunction("_mlir_ciface_kalandra_test_entry_ordinary_v1");
      if (kind == 8 && !fn->hasRetAttribute(Attribute::ZExt))
        return 1;
      if (kind == 9 && fn->hasRetAttribute(Attribute::Range))
        return 1;
      for (auto &block : *fn)
        for (auto &i : block) {
          if (isa<AllocaInst>(i) || isa<LoadInst>(i))
            return 1;
          if (auto *call = dyn_cast<CallBase>(&i);
              call && !call->getCalledFunction()->isIntrinsic())
            return 1;
        }
    }
  }
  {
    std::string chain;
    for (unsigned i = 0; i < 32; ++i)
      chain += "%v" + std::to_string(i) + " = add i64 " +
               (i ? "%v" + std::to_string(i - 1) : "%value") + ", " + std::to_string(i + 1) + "\n";
    std::string ir = R"(
      define signext i64 @generic(i64 signext %seed,i64 signext %rounds,ptr %fault) #0 {
      entry:
        br label %loop
      loop:
        %value=phi i64 [%seed,%entry],[%v31,%step]
        %count=phi i64 [%rounds,%entry],[%next,%step]
        %done=icmp eq i64 %count,0
        br i1 %done,label %exit,label %step
      step:
    )" + chain + R"(
        %next=sub i64 %count,1
        br label %loop
      exit:
        ret i64 %value
      }
      attributes #0 = { "kalandra.ordinary.entry" }
    )";
    LLVMContext ctx;
    SMDiagnostic diagnostic;
    auto module = parseAssemblyString(ir, diagnostic, ctx);
    if (!module || !kalandra::specializeOrdinaryBaseCases(*module, simplify) ||
        verifyModule(*module, &errs()))
      return 1;
    auto *root = module->getFunction("generic");
    if (!root->hasFnAttribute("kalandra.ordinary.base-case") ||
        !root->hasRetAttribute(Attribute::SExt) ||
        kalandra::specializeOrdinaryBaseCases(*module, simplify))
      return 1;
    bool tail = false;
    for (auto &block : *root)
      for (auto &i : block)
        if (auto *call = dyn_cast<CallInst>(&i)) {
          tail |= call->isMustTailCall() && call->hasRetAttr(Attribute::SExt) &&
                  call->paramHasAttr(0, Attribute::SExt);
        }
    if (!tail)
      return 1;
  }

  for (unsigned kind = 0; kind < 9; ++kind) {
    std::string base = kind == 1 || kind == 2 ? "%input"
                       : kind == 3            ? "0x7FF0000000000000"
                       : kind == 4            ? "0x7FF8000000000000"
                                              : "0.0";
    std::string coefficient = kind == 2 || kind == 3 ? "0.0"
                              : kind == 5            ? "0x7FF0000000000000"
                                                     : "5.000000e-01";
    std::string ir = R"(
      define double @countdown(double %input,i64 %n,ptr %fault) #0 {
      entry:
        br label %loop
      loop:
        %count=phi i64 [%n,%entry],[%next,%step]
        %value=phi double [)" +
                     base + R"(,%entry],[%updated,%step]
        switch i64 %count,label %step [i64 )" +
                     (kind == 8 ? "1" : "0") + R"(,label %exit
          i64 -9223372036854775808,label %bad]
      step:
        %next=add nsw i64 %count,)" +
                     (kind == 7 ? "-2" : "-1") + R"(
        %product=fmul double %value,)" +
                     coefficient + R"(
        %updated=fadd double %product,1.0
        br label %loop
      exit:
        %nan=fcmp uno double %value,0.0
        br i1 %nan,label %nanout,label %good
      nanout:
        store i64 0,ptr %fault
        ret double %value
      good:
        ret double %value
      bad:
        store i64 4,ptr %fault
        ret double 0x7FF8000000000000
      }
      attributes #0 = { "kalandra.ordinary.entry" }
    )";
    if(kind==6) {std::size_t at=0;while((at=ir.find("double",at))!=std::string::npos)ir.replace(at,6,"float");}
    LLVMContext context;
    SMDiagnostic diag;
    auto module = parseAssemblyString(ir, diag, context);
    if (!module) {
      diag.print("ordinary-loop", errs());
      return 1;
    }
    auto *fn = module->getFunction("countdown");
    const bool noNan = kind == 0 || kind == 6 || kind == 7 || kind == 8;
    if (kalandra::refineOrdinaryNaNTests(*fn) != noNan) {
      errs() << "NaN proof case " << kind << '\n';
      return 1;
    }
    const bool versions = kind != 7 && kind != 8;
    kalandra::optimizeOrdinaryLoops(*module, simplify);
    if (fn->hasFnAttribute("kalandra.ordinary.countdown") != versions ||
        verifyModule(*module, &errs()))
      return 1;
    if (versions) {
      auto *slow = module->getFunction("countdown.signed-countdown");
      bool minimum = false;
      if (!slow)
        return 1;
      for (auto &block : *slow)
        if (auto *sw = dyn_cast<SwitchInst>(block.getTerminator()))
          for (auto c : sw->cases())
            minimum |= c.getCaseValue()->getValue().isMinSignedValue();
      if (!minimum || kalandra::optimizeOrdinaryLoops(*module, simplify))
        return 1;
    }
  }
  for (std::uint64_t size : {1U, 127U, 128U, 129U, 319U, 575U, 1024U, 2048U, 2049U}) {
    LLVMContext context;
    Module module("pool", context);
    auto *fn = Function::Create(FunctionType::get(Type::getVoidTy(context), false),
                                GlobalValue::InternalLinkage, "unused", module);
    kalandra::OrdinaryPool pool(*fn, *fn, nullptr);
    bool supported = pool.configure(size);
    if (supported != (size <= 2048) ||
        (supported && (pool.bytes < size || pool.bytes % 64 || pool.slots < 2 || pool.slots > 4 ||
                       pool.slots * pool.bytes > 8192)))
      return 1;
  }
  std::cout << "10 ordinary-entry proof/fault/effect/ABI cases, generic loop-free base version, "
               "9 IEEE/countdown cases, pool budgets, and idempotence passed\n";
}
