#include "../lib/BoundedEntry.h"
#include "../lib/UnboundEntry.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include <iostream>
#include <stdexcept>

int main() {
  using namespace llvm;
  for (unsigned kind = 0; kind != 24; ++kind) {
    const bool loop = kind == 1 || kind == 11 || kind == 12;
    std::string code = "%result = mul i64 %n, 17\n br label %finish\n";
    if (loop) code = R"(
      %zero = icmp eq i64 %n, 0
      br i1 %zero, label %finish, label %loop
    loop:
      %left = phi i64 [ %n, %compute ], [ %next, %loop ]
      %sum = phi i64 [ 0, %compute ], [ %value, %loop ]
      %square = mul i64 %sum, %sum
      %value = xor i64 %square, %left
      store i64 %value, ptr %packet
      %next = add i64 %left, -1
      %done = icmp eq i64 %next, 0
      br i1 %done, label %finish, label %loop
    )";
    if (kind == 2) code = "call void @foreign()\n br label %finish\n";
    if (kind == 3) code = "%local = alloca i64\n store volatile i64 %n, ptr %local\n br label %finish\n";
    if (kind == 4) code = "br label %forever\n forever: br label %forever\n";
    if (kind == 5) code = R"(
      br label %outer
    outer:
      %x = phi i64 [ %n, %compute ], [ %xn, %outer.end ]
      %state = phi i64 [ 0, %compute ], [ %mixed, %outer.end ]
      br label %inner
    inner:
      %y = phi i64 [ %n, %outer ], [ %yn, %inner ]
      %acc = phi i64 [ %state, %outer ], [ %mixed, %inner ]
      %yn = add i64 %y, -1
      %sum = mul i64 %y, %x
      %square = mul i64 %acc, %acc
      %mixed = xor i64 %square, %sum
      store i64 %mixed, ptr %packet
      %iy = icmp eq i64 %yn, 0
      br i1 %iy, label %outer.end, label %inner
    outer.end:
      %xn = add i64 %x, -1
      %ix = icmp eq i64 %xn, 0
      br i1 %ix, label %finish, label %outer
    )";
    if (kind == 6) code = R"(
      %choice = icmp slt i64 %n, 0
      br i1 %choice, label %a, label %b
    a:
      %x = phi i64 [ %n, %compute ], [ %yn, %b ]
      %xn = add i64 %x, -1
      %ix = icmp eq i64 %xn, 0
      br i1 %ix, label %finish, label %b
    b:
      %y = phi i64 [ %n, %compute ], [ %xn, %a ]
      %yn = add i64 %y, -2
      %iy = icmp eq i64 %yn, 0
      br i1 %iy, label %finish, label %a
    )";
    if (kind == 7) code = "%memory = call ptr @malloc(i64 %n)\n store ptr %memory, ptr %packet\n br label %finish\n";
    if (kind == 8) code = "call void @llvm.memcpy.p0.p0.i64(ptr %packet, ptr %ctx, i64 %n, i1 false)\n br label %finish\n";
    if (kind == 9) {
      code.clear();
      std::string previous = "%n";
      for (unsigned i = 0; i != 300; ++i) {
        auto value = "%square" + std::to_string(i);
        code += value + " = mul i64 " + previous + ", " + previous + "\n";
        previous = value;
      }
      code += "store i64 " + previous + ", ptr %packet\n br label %finish\n";
    }
    if (kind == 12) code = "%mutated = mul i64 %n, 17\n store i64 %mutated, ptr %np\n" + code;
    if (kind == 13) code = "%value = load volatile i64, ptr %packet\n br label %finish\n";
    if (kind == 14) code = "%wide = zext i64 %n to i128\n %quotient = udiv i128 -1, %wide\n store i128 %quotient, ptr %packet\n br label %finish\n";
    if (kind == 15) code = "call void @kalandra_source_abort_v2(ptr %ctx)\n br label %finish\n";
    if (kind == 17) code = "call void @kalandra_fault_v1(ptr %ctx, i64 4, i64 7, i64 9)\n ret i1 false\n";
    if (kind == 18) code = "call void @kalandra_fault_v1(ptr %ctx, i64 4, i64 7, i64 9)\n store i64 %n, ptr %packet\n ret i1 true\n";
    if (kind == 19) code = "call void @kalandra_fault_v1(ptr %ctx, i64 4, i64 7, i64 9)\n br label %failed\n failed: %out = phi i1 [ false, %compute ]\n ret i1 %out\n";
    if (kind == 20) code = "store i64 %n, ptr %ctx\n br label %finish\n";
    if (kind == 21) code = "%atomic = load atomic i8, ptr %ctx acquire, align 1\n br label %finish\n";
    if (kind == 22) code = "%overlap = getelementptr i8, ptr %packet, i64 20\n store i32 0, ptr %overlap\n br label %finish\n";
    if (kind == 23) code = "call void %ctx()\n br label %finish\n";
    std::string finish = (kind == 4 || kind == 17 || kind == 18 || kind == 19) ? "" : "finish:\n ret i1 true\n";
    std::string ir = R"(
      target triple = "arm64-apple-macosx26.0.0"
      target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
      declare i1 @kalandra_source_run_v4(ptr, ptr, ptr)
      declare void @foreign()
      declare void @kalandra_fault_v1(ptr, i64, i64, i64)
      declare ptr @malloc(i64)
      declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
      declare void @kalandra_source_abort_v2(ptr)
      define i1 @callback(ptr %packet) #0 {
      entry:
        %cp = getelementptr i8, ptr %packet, i64 16
        %ctx = load ptr, ptr %cp
        %mp = getelementptr i8, ptr %MODEBASE, i64 %MODEOFFSET
        %mode = load i8, ptr %mp
        %np = getelementptr i8, ptr %packet, i64 8
        %n = load i64, ptr %np
        switch i8 %mode, label %watched [ i8 0, label %compute i8 1, label %compute ]
      watched:
        call void @foreign()
        ret i1 false
      compute:
    )" + code + finish + R"(
      }
      define i64 @entry(i64 %n, ptr %ctx) {
      start:
        %packet = alloca [256 x i8], align 16
        %np = getelementptr i8, ptr %packet, i64 8
        %INITIALIZE
        %cp = getelementptr i8, ptr %packet, i64 16
        store ptr %ctx, ptr %cp
        %ok = call i1 @kalandra_source_run_v4(ptr %ctx, ptr @callback, ptr %packet)
        %result = zext i1 %ok to i64
        ret i64 %result
      }
      attributes #0 = { %BOUNDARY }
    )";
    auto replace = [&](const std::string &from, const std::string &to) {
      for (auto pos = ir.find(from); pos != std::string::npos; pos = ir.find(from, pos+to.size()))
        ir.replace(pos, from.size(), to);
    };
    replace("%MODEBASE", kind == 10 ? "%packet" : "%ctx");
    replace("%MODEOFFSET", std::to_string(kalandra::runtime::InvocationABI::pollMode));
    replace("%INITIALIZE", kind == 11 ? "" : "store i64 %n, ptr %np");
    replace("%BOUNDARY", kind == 16 ? "nounwind" : "\"kalandra.boundary\"");
    LLVMContext context; SMDiagnostic diagnostic;
    auto module = parseAssemblyString(ir, diagnostic, context);
    if (!module) { diagnostic.print("bounded-entry", errs()); return 1; }
    auto simplify = [](llvm::Function &function, bool) {
      LoopAnalysisManager loops; FunctionAnalysisManager functions;
      CGSCCAnalysisManager calls; ModuleAnalysisManager modules; PassBuilder passes;
      passes.registerLoopAnalyses(loops);
      passes.registerFunctionAnalyses(functions);
      passes.registerCGSCCAnalyses(calls); passes.registerModuleAnalyses(modules);
      passes.crossRegisterProxies(loops, functions, calls, modules);
      auto pipeline = passes.buildFunctionSimplificationPipeline(OptimizationLevel::O2,
                                                                  ThinOrFullLTOPhase::None);
      pipeline.run(function, functions);
    };
    auto unbound = CloneModule(*module);
    const bool unboundChanged = kalandra::addUnboundEntries(*unbound, simplify);
    const bool unboundExpected = kind == 0 || kind == 1 || kind == 4 || kind == 5 || kind == 6 ||
                                 kind == 9 || kind == 12 || kind == 17 || kind == 19;
    if (verifyModule(*unbound, &errs()) || unboundChanged != unboundExpected) {
      errs() << "unbound-entry case " << kind << " changed=" << unboundChanged << '\n';
      unbound->print(errs(), nullptr); return 1;
    }
    if (unboundChanged) {
      bool dispatch = false;
      for (auto &block : *unbound->getFunction("entry")) for (auto &instruction : block)
        if (auto *branch = dyn_cast<CondBrInst>(&instruction); branch)
          if (auto *condition = dyn_cast<ICmpInst>(branch->getCondition()); condition &&
              (condition->getPredicate() == ICmpInst::ICMP_EQ || condition->getPredicate() == ICmpInst::ICMP_NE) &&
              condition->getOperand(0)->getType()->isIntegerTy(8)) dispatch = true;
      if (!dispatch) { errs() << "CASE " << kind << "\n"; unbound->print(errs(), nullptr); throw std::runtime_error("unbound entry lost its context-mode guard"); }
    }
    if (kind >= 17) continue;
    const bool changed = kalandra::addBoundedEntries(*module, simplify);
    if (verifyModule(*module, &errs()) || changed != (kind == 0 || kind == 1)) {
      errs() << "bounded-entry case " << kind << " changed=" << changed << '\n';
      module->print(errs(), nullptr); return 1;
    }
    for (auto &block : *module->getFunction("entry")) for (auto &instruction : block)
      if (auto *call = dyn_cast<CallBase>(&instruction); call && call->getCalledFunction())
        if (call->getCalledFunction()->getName().contains(".bounded"))
          throw std::runtime_error("bounded callback was not inlined");
  }
  std::cout << "17 bounded and 24 unbound entry admission/rejection cases passed\n";
}
