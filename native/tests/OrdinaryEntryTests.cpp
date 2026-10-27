#include "../lib/OrdinaryBaseCase.h"
#include "../lib/OrdinaryBufferDirect.h"
#include "../lib/OrdinaryDirect.h"
#include "../lib/OrdinaryEntry.h"
#include "../lib/OrdinaryInputs.h"
#include "../lib/OrdinaryLoops.h"
#include "../lib/OrdinaryMain.h"
#include "../lib/OrdinaryPair.h"
#include "../lib/OrdinaryReductions.h"
#include "../lib/OrdinaryScalarRanges.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include <bit>
#include <cstring>
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

int main(int argc, char **argv) {
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
    auto *work = module->getFunction("generic.work");
    if (!work || !work->hasParamAttribute(0, Attribute::Range) ||
        root->hasParamAttribute(0, Attribute::Range) ||
        work->getParamAttribute(0, Attribute::Range).getRange().contains(APInt(64, 0)))
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

  // Exact floating folding requires both a checked input domain and exact
  // intermediate operations; unknown, signed-zero and rounded cases reject it.
  for (unsigned kind = 0; kind < 19; ++kind) {
    LLVMContext ctx;
    Module module("exact", ctx);
    IRBuilder<> b(ctx);
    Type *type = kind == 1 ? b.getFloatTy() : b.getDoubleTy();
    auto *fn = Function::Create(FunctionType::get(type, {b.getInt64Ty(), b.getInt64Ty()}, false),
                                GlobalValue::ExternalLinkage, "exact", module);
    b.SetInsertPoint(BasicBlock::Create(ctx, "entry", fn));
    for (unsigned n = 0; n < 2; ++n)
      if (kind != 2)
        fn->addParamAttr(n, Attribute::get(ctx, Attribute::Range,
                                           ConstantRange(APInt(64, kind == 4 ? -1 : 0),
                                                         APInt(64, kind == 3 ? 65537 : 65536))));
    Value *integer = fn->getArg(0);
    if (kind == 13 || kind == 14 || kind == 15)
      integer = b.CreateTrunc(integer, b.getIntNTy(kind == 14 ? 17 : 16));
    Value *input =
        (kind == 12 || kind == 13) ? b.CreateUIToFP(integer, type) : b.CreateSIToFP(integer, type);
    if (kind == 11)
      input = b.CreateFAdd(input, ConstantFP::get(type, 1.0));
    double coefficient = kind == 5   ? -.25
                         : kind == 6 ? 0
                         : kind == 7 ? 1.1
                         : kind == 8 ? std::ldexp(1., 1023)
                         : kind == 9 ? std::ldexp(1., -1074)
                                     : .25;
    auto *first = b.CreateFMul(input, ConstantFP::get(type, coefficient));
    Value *other = kind == 10 ? b.CreateSIToFP(fn->getArg(1), type) : input;
    auto *second = b.CreateFMul(other, ConstantFP::get(type, .125));
    Value *value = kind == 16   ? b.CreateFSub(first, second)
                   : kind == 17 ? b.CreateFDiv(first, second)
                                : b.CreateFAdd(first, second);
    if (kind == 18)
      value = b.CreateFAdd(input, b.CreateFMul(input, ConstantFP::get(type, std::ldexp(1., -53))));
    b.CreateRet(value);
    const bool expected = kind == 0 || kind == 1 || kind == 12 || kind == 13 || kind == 14;
    if (bool(kalandra::ordinaryExactScale(value)) != expected || verifyModule(module, &errs())) {
      errs() << "exact proof " << kind << '\n';
      return 1;
    }
  }
  auto exhaustive = []<class T>() {
    for (unsigned i = 0; i < 65536; ++i) {
      T x = T(i);
      volatile T a = x * T(.25), b = a * T(.5), c = x * T(.0625);
      T original = b + c, folded = x * T(.1875);
      if (std::memcmp(&original, &folded, sizeof(T)))
        return false;
      volatile T d = x * T(.015625), e = d * T(1.015625), f = x * T(.00390625);
      original = e + f;
      folded = x * T(.019775390625);
      if (std::memcmp(&original, &folded, sizeof(T)))
        return false;
    }
    return true;
  };
  if (!exhaustive.operator()<float>() || !exhaustive.operator()<double>())
    return 1;
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

  // Public Win64 entries return a scalar. The aggregate helper is private and
  // can be consumed only by LLVM-to-LLVM calls, never a host C++ declaration.
  {
    LLVMContext context;
    SMDiagnostic diagnostic;
    std::string ir = R"(
      target triple = "x86_64-pc-windows-msvc"
      target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n8:16:32:64-S128"
      attributes #0 = { "kalandra.ordinary.entry" }
    )";
    unsigned index = 0;
    for (auto type :
         {"i1", "i8", "i16", "i32", "i64", "i8", "i16", "i32", "i64", "float", "double"}) {
      const std::string name = "win" + std::to_string(index++);
      ir += "define " + std::string(type) + " @" + name + "_ordinary_v1(" + type + " %a, " + type +
            " %b, ptr %fault) #0 { ret " + type + " %a }\n";
    }
    ir += R"(
      define double @_mlir_ciface_kalandra_float64_entry_ordinary_v1(ptr %fault) #0 { ret double 1.0 }
    )";
    auto module = parseAssemblyString(ir, diagnostic, context);
    if (!module)
      return 1;
    for (unsigned n : {0u, 1u, 2u, 5u, 6u}) {
      auto *fn = module->getFunction("win" + std::to_string(n) + "_ordinary_v1");
      const auto extension = (n == 1 || n == 2) ? Attribute::SExt : Attribute::ZExt;
      fn->addRetAttr(extension);
      fn->addParamAttr(0, extension);
      fn->addParamAttr(1, extension);
    }
    if (!module || !kalandra::addOrdinaryPairEntries(*module, simplify) ||
        !kalandra::addOrdinaryDirectEntries(*module, simplify))
      return 1;
    for (unsigned n = 0; n < index; ++n) {
      auto prefix = "win" + std::to_string(n);
      auto *scalar = module->getFunction(prefix + "_ordinary_v1");
      auto *pair = module->getFunction(prefix + "_ordinary_pair_v1");
      auto *direct = module->getFunction(prefix + "_ordinary_direct_v1");
      if (!pair || !pair->hasInternalLinkage() || !direct || !direct->hasExternalLinkage() ||
          direct->getReturnType() != scalar->getReturnType() || direct->arg_size() != 2 ||
          verifyFunction(*direct, &errs()))
        return 1;
    }
    auto *main = kalandra::addOrdinaryMain(*module);
    if (!main || verifyModule(*module, &errs()))
      return 1;
    bool selected = false;
    for (auto &block : *main)
      for (auto &i : block)
        if (auto *call = dyn_cast<CallInst>(&i);
            call && call->getCalledFunction() &&
            call->getCalledFunction()->getName() ==
                "_mlir_ciface_kalandra_float64_entry_ordinary_direct_v1")
          selected = true;
    if (!selected || argc > 2)
      return 1;
    if (argc == 2) {
      std::error_code error;
      raw_fd_ostream output(argv[1], error);
      if (error)
        return 1;
      module->print(output, nullptr);
    }
  }
  for (unsigned kind = 0; kind < 4; ++kind) {
    LLVMContext context;
    SMDiagnostic diagnostic;
    std::string transfer =
        kind == 1 ? "%v = call i64 @buffer_ordinary_v1.worker(i64 %a, ptr %fault)\n %sum = add i64 "
                    "%v, 1\n ret i64 %sum"
                  : "%v = tail call i64 @buffer_ordinary_v1.worker(i64 %a, ptr %fault)\n br label "
                    "%done\n done:\n %value=phi i64 [%v,%entry]\n ret i64 %value";
    auto ir = std::string(R"(
      target triple = "x86_64-pc-windows-msvc"
      target datalayout = "e-m:w-i64:64-i128:128-n8:16:32:64-S128"
      define i64 @buffer_ordinary_v1(i64 %a,ptr %fault) #0 {
      entry:
      )") + transfer +
              R"(
      }
      define internal i64 @buffer_ordinary_v1.worker(i64 %a,ptr %fault) #1 {
        %result=call i64 @buffer_ordinary_heap_v1(i64 %a,ptr %fault)
        ret i64 %result
      }
      declare void @kalandra_ordinary_begin_v1(ptr)
      declare void @kalandra_ordinary_end_v1(ptr,ptr)
      define internal i64 @buffer_ordinary_heap_v1(i64 %a,ptr %fault) #2 {
        %context=alloca [64 x i8]
        call void @kalandra_ordinary_begin_v1(ptr %context)
      )" + (kind == 2 ? "" : "call void @kalandra_ordinary_end_v1(ptr %context,ptr %fault)\n") +
              R"(
        ret i64 %a
      }
      attributes #0 = { "kalandra.ordinary.entry" "kalandra.ordinary.local-buffers" }
      attributes #1 = { noinline "kalandra.ordinary.local-buffers" }
      attributes #2 = { noinline "kalandra.ordinary.fallback" }
      )";
    if (kind == 3) {
      auto at = ir.find("[64 x i8]");
      ir.replace(at, std::string("[64 x i8]").size(), "[32768 x i8]");
    }
    auto module = parseAssemblyString(ir, diagnostic, context);
    if (!module) {
      diagnostic.print("buffer proof", errs());
      return 1;
    }
    bool changed = kalandra::addOrdinaryBufferDirectEntries(*module, simplify);
    if (changed != (kind == 0) || verifyModule(*module, &errs()))
      return 1;
    if (!changed)
      continue;
    auto *direct = module->getFunction("buffer_ordinary_direct_v1");
    unsigned tails = 0;
    for (auto &block : *direct)
      for (auto &i : block)
        if (auto *call = dyn_cast<CallInst>(&i); call && call->isMustTailCall())
          ++tails;
    if (tails != 1 || kalandra::addOrdinaryBufferDirectEntries(*module, simplify))
      return 1;
  }
  std::cout << "10 ordinary-entry proof/fault/effect/ABI cases, generic loop-free base version, "
               "9 IEEE/countdown cases, 19 exact floating proof cases and 262144 exhaustive "
               "values, 19 deferred reduction cases, 3 guarded-input cases, pool "
               "budgets, Win64 scalar ABI, buffer tail/settlement proofs and idempotence "
               "passed\n";
}
