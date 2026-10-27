#include "../lib/OrdinaryBaseCase.h"
#include "../lib/OrdinaryEntry.h"
#include "../lib/OrdinaryInputs.h"
#include "../lib/OrdinaryLoops.h"
#include "../lib/OrdinaryMain.h"
#include "../lib/OrdinaryReductions.h"
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
    if (kind == 6) {
      std::size_t at = 0;
      while ((at = ir.find("double", at)) != std::string::npos)
        ir.replace(at, 6, "float");
    }
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

  for (unsigned kind = 0; kind < 19; ++kind) {
    std::string ir = R"(
      @outside = global i64 0
      declare void @foreign_effect()
      define i64 @reduce(i64 signext %seed,ptr %fault) #0 {
      entry:
        %storage=alloca [4 x i64],align 8
        %p0=getelementptr [4 x i64],ptr %storage,i64 0,i64 0
        %p1=getelementptr [4 x i64],ptr %storage,i64 0,i64 1
        %p2=getelementptr [4 x i64],ptr %storage,i64 0,i64 2
        %p3=getelementptr [4 x i64],ptr %storage,i64 0,i64 3
        store i64 %seed,ptr %p0
        store i64 %seed,ptr %p1
        store i64 %seed,ptr %p2
        store i64 %seed,ptr %p3
        BEFORE
        br label %loop
      loop:
        %acc=phi i64 [INITIAL,%entry],[%out,%latch]
        %index=phi i64 [0,%entry],[%next,%latch]
        %pointer=getelementptr [4 x i64],ptr BASE,i64 0,i64 %index
        %value=load VOLATILE i64,ptr %pointer,align 8
        EFFECT
        %a=sext i64 %acc to i128
        %v=sext i64 %value to i128
        %sum=add nsw i128 %a,%v
        %biased=add i128 %sum,-9223372036854775808
        %bad=icmp ult i128 %biased,BOUND
        br i1 %bad,label %faulted,label %latch
      latch:
        %out=trunc i128 %sum to i64
        %next=add i64 %index,STEP
        %done=icmp eq i64 %next,LIMIT
        br i1 %done,label %exit,label %loop
      exit:
        ret i64 %out
      faulted:
        store i64 4,ptr %fault
        ret i64 -9223372036854775808
      }
      attributes #0 = { "kalandra.ordinary.entry" "kalandra.ordinary.local-buffers" }
    )";
    auto replace = [&](StringRef from, StringRef to) {
      auto at = ir.find(from.str());
      if (at != std::string::npos)
        ir.replace(at, from.size(), to.str());
    };
    replace("BEFORE", kind == 1   ? "store i64 0,ptr %fault"
                      : kind == 2 ? "store i64 0,ptr @outside"
                                  : "");
    replace("INITIAL", kind == 3 ? "1" : "0");
    replace("VOLATILE", kind == 4 ? "volatile" : "");
    replace("EFFECT", kind == 5   ? "store i64 0,ptr %pointer"
                      : kind == 6 ? "call void @foreign_effect()"
                                  : "");
    replace("BASE", kind == 7 ? "%fault" : "%storage");
    replace("BOUND", kind == 8 ? "-18446744073709551615" : "-18446744073709551616");
    replace("STEP", kind == 9 ? "2" : "1");
    replace("LIMIT", kind == 10 ? "65537" : kind == 11 ? "0" : "4");
    if (kind == 13)
      replace("i64 %index\n        %value", "i64 %acc\n        %value");
    if (kind == 14)
      replace("%sum=add nsw i128 %a,%v",
              "%divide=sdiv i128 10,%v\n        %sum=add nsw i128 %a,%v");
    if (kind == 15)
      replace("%index=phi", "%other=phi i64 [0,%entry],[1,%latch]\n        %index=phi");
    if (kind == 16)
      replace("icmp eq i64 %next,4", "icmp eq i64 %next,8");
    if (kind == 17) {
      replace("%value=load  i64,ptr %pointer,align 8",
              "%value=load  i64,ptr %pointer,align 16, !noundef !0");
      ir += "\n!0 = !{}\n";
    }
    LLVMContext ctx;
    SMDiagnostic diagnostic;
    auto module = parseAssemblyString(ir, diagnostic, ctx);
    if (!module) {
      diagnostic.print("reduction-proof", errs());
      return 1;
    }
    if (kind == 18)
      module->setDataLayout("e-p:32:32");
    const bool expected = kind == 0 || kind == 12 || kind == 17;
    auto *original = module->getFunction("reduce");
    if (kind == 12)
      original->addRetAttr(Attribute::SExt);
    const bool changed = kalandra::specializeOrdinaryReductions(*module, simplify);
    if (changed != expected || verifyModule(*module, &errs())) {
      errs() << "reduction proof " << kind << " changed=" << changed << '\n';
      return 1;
    }
    if (changed) {
      auto *fn = module->getFunction("reduce");
      if (!fn->hasFnAttribute("kalandra.ordinary.reduction") ||
          kalandra::specializeOrdinaryReductions(*module, simplify))
        return 1;
      if (kind == 17)
        for (auto &block : *fn)
          for (auto &i : block)
            if (auto *load = dyn_cast<LoadInst>(&i);
                load && load->getMetadata(LLVMContext::MD_noundef))
              return 1;
      bool replay = false;
      for (auto &block : *fn)
        for (auto &i : block)
          if (auto *call = dyn_cast<CallInst>(&i); call && call->isMustTailCall())
            replay |= call->getCalledFunction() == original &&
                      call->paramHasAttr(0, Attribute::SExt) &&
                      (kind != 12 || call->hasRetAttr(Attribute::SExt));
      if (!replay)
        return 1;
    }
  }

  for (unsigned kind = 0; kind < 3; ++kind) {
    std::string ir = "define signext i64 @inputs(i64 signext %seed,ptr %fault) #0 {\nentry:\n";
    unsigned count = kind == 1 ? 15 : 24;
    for (unsigned i = 0; i < count; ++i) {
      if (i)
        ir += "next" + std::to_string(i) + ":\n";
      ir += "%c" + std::to_string(i) + " = icmp eq i64 %seed," +
            std::to_string(0x7fffffffffffffffULL - i) + "\n";
      ir +=
          "br i1 %c" + std::to_string(i) + ",label %bad,label %next" + std::to_string(i + 1) + "\n";
    }
    ir += "next" + std::to_string(count) +
          ":\n%out=add i64 %seed,7\nret i64 %out\nbad:\nstore i64 4,ptr %fault\nret i64 "
          "-9223372036854775808\n}\n";
    ir += "attributes #0 = { \"kalandra.ordinary.entry\" \"kalandra.ordinary.local-buffers\" "
          "\"kalandra.ordinary.reduction\" }\n";
    LLVMContext ctx;
    SMDiagnostic diagnostic;
    auto module = parseAssemblyString(ir, diagnostic, ctx);
    if (!module) {
      diagnostic.print("input-proof", errs());
      return 1;
    }
    if (kind == 2)
      module->getFunction("inputs")->removeFnAttr("kalandra.ordinary.reduction");
    const bool changed = kalandra::specializeOrdinaryInputs(*module, simplify);
    if (changed != (kind == 0) || verifyModule(*module, &errs())) {
      errs() << "input proof " << kind << '\n';
      return 1;
    }
    auto *fn = module->getFunction("inputs");
    if (fn->hasParamAttribute(0, Attribute::Range) || !fn->hasRetAttribute(Attribute::SExt) ||
        kalandra::specializeOrdinaryInputs(*module, simplify))
      return 1;
  }
  std::cout << "10 ordinary-entry proof/fault/effect/ABI cases, generic loop-free base version, "
               "9 IEEE/countdown cases, 19 deferred reduction cases, 3 guarded-input cases, pool "
               "budgets, and idempotence "
               "passed\n";
}
