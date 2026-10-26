#include "../lib/IntegerRanges.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/SourceMgr.h"
#include <iostream>
#include <stdexcept>

int main() {
  using namespace llvm;
  for (unsigned kind = 0; kind < 8; ++kind) {
    const bool guarded = kind != 1 && kind != 4;
    std::string prefix = guarded ? R"(
      %low = icmp sge i64 %seed, -100
      %high = icmp sle i64 %seed, 100
      %safe = and i1 %low, %high
      br i1 %safe, label %checked, label %rejected
    rejected:
      ret i1 false
    checked:
    )" : "";
    if (kind == 2) prefix = R"(
      br i1 %mode, label %test, label %checked
    test:
      %low = icmp sge i64 %seed, -100
      %high = icmp sle i64 %seed, 100
      %safe = and i1 %low, %high
      br i1 %safe, label %checked, label %rejected
    rejected:
      ret i1 false
    checked:
    )";
    if (kind == 5) prefix = R"(
      %low = icmp sge i64 %seed, 9223372036854775804
      br i1 %low, label %checked, label %rejected
    rejected:
      ret i1 false
    checked:
    )";
    const auto predecessor = guarded || kind == 2 ? "checked" : "entry";
    std::string ir = R"(
      declare void @llvm.assume(i1)
      declare void @may_throw()
      define i1 @probe(i64 %seed, i64 %value, i64 %rounds, ptr %memory, i1 %mode) {
      entry:
        %offset = add i64 %seed, 7
      )" + prefix + R"(
        br label %loop
      loop:
        %cursor = phi i64 [ %value, %PREDECESSOR ], [ %next, %loop ]
        %count = phi i64 [ %rounds, %PREDECESSOR ], [ %decrement, %loop ]
        %changing = phi i64 [ %offset, %PREDECESSOR ], [ %loaded, %loop ]
        %loaded = load volatile i64, ptr %memory
        %bounded = srem i64 %cursor, 1000003
        %left = sext i64 %bounded to i128
        %right = sext i64 %SOURCE to i128
        %sum = add i128 %left, %right
        %shift = add i128 %sum, -9223372036854775808
        %overflow = icmp ult i128 %shift, -18446744073709551616
        FUTURE
        %next = add i64 %cursor, 17
        %decrement = add i64 %count, -1
        %again = icmp ne i64 %decrement, 0
        br i1 %again, label %loop, label %done
      done:
        ret i1 %overflow
      }
    )";
    auto replace = [&](StringRef from, StringRef to) {
      std::size_t at = 0;
      while ((at = ir.find(from.str(), at)) != std::string::npos) {
        ir.replace(at, from.size(), to.str()); at += to.size();
      }
    };
    replace("PREDECESSOR", predecessor);
    replace("SOURCE", kind == 3 ? "changing" : kind == 6 ? "loaded" : "offset");
    replace("FUTURE", kind == 4 ? R"(
      store i1 %overflow, ptr %memory
      call void @may_throw()
      %future_low = icmp sge i64 %seed, -100
      %future_high = icmp sle i64 %seed, 100
      %future_safe = and i1 %future_low, %future_high
      call void @llvm.assume(i1 %future_safe)
    )" : "");
    if (kind == 7)
      replace("icmp ult i128 %shift, -18446744073709551616",
              "icmp sge i128 %sum, -9223372036854775808");
    LLVMContext context;
    SMDiagnostic error;
    auto module = parseAssemblyString(ir, error, context);
    if (!module) { error.print("ranges", errs()); return 1; }
    if (verifyModule(*module, &errs())) return 1;
    auto &fn = *module->getFunction("probe");
    FunctionAnalysisManager analyses;
    PassBuilder passes; passes.registerFunctionAnalyses(analyses);
    auto &lvi = analyses.getResult<LazyValueAnalysis>(fn);
    auto &dom = analyses.getResult<DominatorTreeAnalysis>(fn);
    kalandra::foldProvenIntegerComparisons(fn, lvi, dom);
    if (verifyModule(*module, &errs())) return 1;
    auto *result = cast<ReturnInst>(fn.back().getTerminator())->getReturnValue();
    auto *constant = dyn_cast<ConstantInt>(result);
    const bool proven = kind == 0 || kind == 7;
    if (bool(constant) != proven || (constant && constant->isOne() != (kind == 7)))
      throw std::runtime_error("unsound/missed integer range proof, case " + std::to_string(kind));
  }
  std::cout << "Integer ranges preserve unknown captures, sibling facts, changing PHIs, "
               "future assumptions, wrapping arithmetic and fresh memory loads\n";
}
