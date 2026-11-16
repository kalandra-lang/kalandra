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
  for (unsigned kind = 0; kind < 10; ++kind) {
    std::string prefix = kind == 4 ? R"(
      br i1 %mode, label %test, label %checked
    test:
    )" : "";
    std::string ir = R"(
      define void @induction(i64 %count, ptr %memory, i1 %mode) {
      entry:
    )" + prefix + R"(
        %low = icmp sge i64 %count, 0
        %high = icmp sle i64 %count, 1152921504606846975
        %valid = and i1 %low, %high
        br i1 %valid, label %checked, label %done
      checked:
        br label %loop
      loop:
        %cursor = phi i64 [ SEED, %checked ], [ %next, %body ]
        ENTER
      body:
        %left = sext i64 %count to i128
        %right = sext i64 %cursor to i128
        %calculated = MATH i128 %left, %right
        %shifted = add i128 %calculated, -9223372036854775808
        %overflow = icmp ult i128 %shifted, -18446744073709551616
        store volatile i1 %overflow, ptr %memory
        OTHERTEST
        NEXT
        REPEAT
      done:
        ret void
      }
    )";
    auto replace = [&](StringRef from, StringRef to) {
      auto at = ir.find(from.str());
      ir.replace(at, from.size(), to.str());
    };
    replace("SEED", kind == 1 ? "-9223372036854775808" :
                    (kind == 5 || kind == 6 ? "1" : "0"));
    replace("MATH", kind == 4 || kind == 7 || kind == 8 ? "add" : "sub");
    replace("ENTER", kind == 8 ? "br label %body" :
                      "%active = icmp slt i64 %cursor, %count\n"
                      "br i1 %active, label %body, label %done");
    replace("REPEAT", kind == 8 ? "%again = icmp slt i64 %next, %count\n"
                       "br i1 %again, label %loop, label %done" : "br label %loop");
    if (kind == 9)
      replace("%cursor = phi i64 [ 0, %checked ], [ %next, %body ]",
              "%cursor = phi i64 [ 0, %checked ], [ %other, %body ]\n"
              "%other = phi i64 [ %cursor, %body ], [ -9223372036854775808, %checked ]");
    replace("OTHERTEST", kind == 9 ?
            "%otherwide = sext i64 %other to i128\n"
            "%otherdiff = sub i128 %left, %otherwide\n"
            "%othershift = add i128 %otherdiff, -9223372036854775808\n"
            "%otheroverflow = icmp ult i128 %othershift, -18446744073709551616\n"
            "store volatile i1 %otheroverflow, ptr %memory" : "");
    replace("NEXT", kind == 2 ? "%next = add i64 %cursor, -9223372036854775808" :
                    kind == 3 ? "%next = load volatile i64, ptr %memory" :
                    kind == 5 ? "%poisonable = add nsw i64 %cursor, 9223372036854775807\n"
                                "%next = freeze i64 %poisonable" :
                    kind == 6 ? "%wide = sext i64 %cursor to i128\n"
                                "%doubled = add nsw i128 %wide, %wide\n"
                                "%next = trunc nsw i128 %doubled to i64" :
                                "%next = add nsw i64 %cursor, 1");
    LLVMContext context;
    SMDiagnostic error;
    auto module = parseAssemblyString(ir, error, context);
    if (!module) { error.print("induction-ranges", errs()); return 1; }
    if (verifyModule(*module, &errs())) return 1;
    auto &fn = *module->getFunction("induction");
    FunctionAnalysisManager analyses;
    PassBuilder passes; passes.registerFunctionAnalyses(analyses);
    auto &lvi = analyses.getResult<LazyValueAnalysis>(fn);
    auto &dom = analyses.getResult<DominatorTreeAnalysis>(fn);
    kalandra::foldProvenIntegerComparisons(fn, lvi, dom);
    if (verifyModule(*module, &errs())) return 1;
    bool removed = false, constant = false;
    for (auto &block : fn)
      for (auto &instruction : block)
        if (auto *store = dyn_cast<StoreInst>(&instruction))
          if (auto *value = dyn_cast<ConstantInt>(store->getValueOperand())) {
            constant = true;
            removed |= value->isZero();
          }
    const bool safe = kind == 0 || kind == 6 || kind == 7 || kind == 8;
    if (safe ? !removed : constant)
      throw std::runtime_error("unsound/missed induction range proof, case " + std::to_string(kind));
  }
  std::cout << "Integer ranges preserve unknown captures, sibling facts, changing PHIs, "
               "future assumptions, wrapping arithmetic and fresh memory loads\n";
}
