// Transform real Kalandra-generated LLVM for isolated mechanism experiments.
// This is not a production cancellation placement pass.
#include "Control.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
using namespace llvm;
using kalandra::experiment::Control;

int main(int argc, char **argv) {
  if (argc != 4 && argc != 5)
    return 2;
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseIRFile(argv[1], diagnostic, context);
  if (!module) {
    diagnostic.print(argv[0], errs());
    return 1;
  }
  const std::string mode = argv[3];
  const bool plain = mode == "floor" || mode == "current";
  const bool asynchronous = mode == "async" || mode == "async-unwind" || mode == "hybrid";
  const bool page = mode == "page";
  const bool guard = mode == "guard";
  const bool budget = mode == "budget";
  if (!plain)
    if (auto *boundary = module->getFunction("kalandra_source_run_v3"))
      boundary->setName("kalandra_cancel_source_run_v3");
  if (!plain && !asynchronous && !page && !budget && !guard && mode != "entry" &&
      mode != "cycles" && mode != "unwind-poll")
    throw std::runtime_error("unknown experiment");
  if (!module->getTargetTriple().isAArch64() || !module->getTargetTriple().isOSDarwin())
    throw std::runtime_error("first mechanism probe requires Darwin ARM64");
  auto *ptr = PointerType::getUnqual(context);
  auto *i64 = Type::getInt64Ty(context);
  auto *i32 = Type::getInt32Ty(context);
  auto *regionType = StructType::get(ptr, ptr);
  std::vector<Constant *> regions;
  auto stop = module->getOrInsertFunction(mode == "unwind-poll" ? "kalandra_cancel_unwind"
                                                                : "kalandra_cancel_jump",
                                          Type::getVoidTy(context));
  cast<Function>(stop.getCallee())->addFnAttr(Attribute::NoReturn);
  std::map<std::string, std::uint64_t> extents;
  if (argc == 5) {
    std::ifstream input(argv[4]);
    std::string name;
    std::uint64_t size;
    while (input >> name >> size)
      extents[name] = size;
  }
  unsigned points = 0;
  for (auto &fn : *module) {
    if (fn.isDeclaration())
      continue;
    if (!plain) {
      auto features = fn.getFnAttribute("target-features").getValueAsString().str();
      if (!features.empty())
        features += ',';
      features += "+reserve-x28";
      fn.addFnAttr("target-features", features);
      fn.addFnAttr(Attribute::NoRedZone);
      fn.addFnAttr("frame-pointer", "all");
      fn.removeFnAttr(Attribute::NoUnwind);
      fn.setUWTableKind(UWTableKind::Async);
    }
    if (plain || !(fn.getName().contains(".body.clear") || fn.getName().ends_with(".boundary")))
      continue;
    // Export bodies so the linked image's symbol extents can be verified.
    fn.setLinkage(GlobalValue::ExternalLinkage);
    fn.setVisibility(GlobalValue::HiddenVisibility);
    fn.removeFnAttr(Attribute::AlwaysInline);
    fn.addFnAttr(Attribute::NoInline);
    SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 16> backedges;
    FindFunctionBackedges(fn, backedges);
    std::set<BasicBlock *> pollBlocks;
    for (auto [from, to] : backedges)
      pollBlocks.insert(const_cast<BasicBlock *>(from));
    // Conservatively cover every member of a direct-call cycle, including
    // mutual recursion. Indirect dispatch remains an entry safepoint.
    std::set<const Function *> visited;
    std::function<bool(const Function *)> reachesSelf = [&](const Function *callee) {
      if (!callee || callee == &fn)
        return true;
      if (callee->isDeclaration() || !visited.insert(callee).second)
        return false;
      for (const auto &b : *callee)
        for (const auto &instruction : b)
          if (auto *call = dyn_cast<CallBase>(&instruction); call && !call->isInlineAsm())
            if (reachesSelf(call->getCalledFunction()))
              return true;
      return false;
    };
    bool recursive = false;
    for (auto &block : fn)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallBase>(&instruction); call && !call->isInlineAsm())
          recursive |= reachesSelf(call->getCalledFunction());
    if (recursive || mode == "entry" || mode == "unwind-poll")
      pollBlocks.insert(&fn.getEntryBlock());
    std::vector<BasicBlock *> original;
    for (auto &block : fn)
      original.push_back(&block);
    if (guard) {
      for (auto &block : fn) {
        auto *branch = dyn_cast<BranchInst>(block.getTerminator());
        if (!branch || !branch->getMetadata("kalandra.physical.guard"))
          continue;
        auto *compare = dyn_cast<ICmpInst>(branch->getCondition());
        if (!compare || !isa<LoadInst>(compare->getOperand(1)))
          throw std::runtime_error("unexpected stack guard shape");
        IRBuilder<> ir(compare);
        auto *type = FunctionType::get(ptr, {}, false);
        auto *state = ir.CreateCall(type, InlineAsm::get(type, "mov $0, x28", "=r", false), {});
        auto *address = ir.CreateConstGEP1_64(ir.getInt8Ty(), state, offsetof(Control, stackLimit));
        auto *limit = ir.CreateLoad(i64, address);
        limit->setAtomic(AtomicOrdering::Acquire);
        compare->setOperand(1, limit);
        // Cancellation reuses the existing stack-guard branch. A real exhausted
        // stack retains the language's original fault path.
        auto *failure = branch->getSuccessor(1);
        auto *dispatch = BasicBlock::Create(context, "cancel.guard", &fn);
        branch->setSuccessor(1, dispatch);
        ir.SetInsertPoint(dispatch);
        auto *requested = ir.CreateLoad(
            i32, ir.CreateConstGEP1_64(ir.getInt8Ty(), state, offsetof(Control, requested)));
        requested->setAtomic(AtomicOrdering::Acquire);
        auto *abort = BasicBlock::Create(context, "cancel.guard.stop", &fn);
        ir.CreateCondBr(ir.CreateICmpNE(requested, ir.getInt32(0)), abort, failure);
        ir.SetInsertPoint(abort);
        ir.CreateCall(stop);
        ir.CreateUnreachable();
        ++points;
      }
      // Stack checks cover growing recursion. Tail/CFG loops still need a probe.
      pollBlocks.erase(&fn.getEntryBlock());
    }
    if (!asynchronous) {
      for (auto *block : original) {
        if (!pollBlocks.contains(block))
          continue;
        auto insert = block == &fn.getEntryBlock() ? block->getFirstInsertionPt()
                                                   : block->getTerminator()->getIterator();
        // Preserve LLVM's musttail adjacency contract.
        if (auto *ret = dyn_cast<ReturnInst>(block->getTerminator()))
          if (auto *tail = dyn_cast_or_null<CallInst>(ret->getPrevNode());
              tail && tail->isMustTailCall())
            insert = tail->getIterator();
        auto *continuation = block->splitBasicBlock(insert, "cancel.continue");
        block->getTerminator()->eraseFromParent();
        IRBuilder<> ir(block);
        auto *regType = FunctionType::get(ptr, {}, false);
        auto *state =
            ir.CreateCall(regType, InlineAsm::get(regType, "mov $0, x28", "=r", false), {});
        auto field = [&](std::size_t offset) {
          return ir.CreateConstGEP1_64(ir.getInt8Ty(), state, offset);
        };
        if (page) {
          auto *address = ir.CreateLoad(ptr, field(offsetof(Control, page)));
          auto *type = FunctionType::get(ir.getVoidTy(), {ptr}, false);
          ir.CreateCall(type, InlineAsm::get(type, "ldr wzr, [$0]", "r,~{memory}", true),
                        {address});
          ir.CreateBr(continuation);
        } else {
          auto *probe = block;
          if (budget) {
            auto *credit =
                ir.CreateSub(ir.CreateLoad(i64, field(offsetof(Control, credit))), ir.getInt64(1));
            ir.CreateStore(credit, field(offsetof(Control, credit)));
            probe = BasicBlock::Create(context, "cancel.budget", &fn);
            ir.CreateCondBr(ir.CreateICmpEQ(credit, ir.getInt64(0)), probe, continuation);
            ir.SetInsertPoint(probe);
            ir.CreateStore(ir.getInt64(256), field(offsetof(Control, credit)));
          }
          auto *flag = ir.CreateLoad(i32, field(offsetof(Control, requested)));
          flag->setAtomic(AtomicOrdering::Acquire);
          auto *abort = BasicBlock::Create(context, "cancel.stop", &fn);
          ir.CreateCondBr(ir.CreateICmpEQ(flag, ir.getInt32(0)), continuation, abort);
          ir.SetInsertPoint(abort);
          ir.CreateCall(stop);
          ir.CreateUnreachable();
        }
        ++points;
      }
    }
    if (mode == "hybrid") {
      std::vector<Instruction *> returns;
      for (auto &block : fn)
        for (auto &instruction : block)
          if (auto *call = dyn_cast<CallInst>(&instruction);
              call && call->getCalledFunction() && !call->doesNotReturn() &&
              call->getCalledFunction()->isDeclaration() &&
              call->getCalledFunction()->getName().starts_with("kalandra_"))
            returns.push_back(call->getNextNode());
      for (auto *position : returns) {
        auto *block = position->getParent();
        auto *continuation =
            block->splitBasicBlock(position->getIterator(), "cancel.runtime.return");
        block->getTerminator()->eraseFromParent();
        IRBuilder<> ir(block);
        auto *type = FunctionType::get(ptr, {}, false);
        auto *state = ir.CreateCall(type, InlineAsm::get(type, "mov $0, x28", "=r", false), {});
        auto *flag = ir.CreateLoad(
            i32, ir.CreateConstGEP1_64(ir.getInt8Ty(), state, offsetof(Control, requested)));
        flag->setAtomic(AtomicOrdering::Acquire);
        auto *abort = BasicBlock::Create(context, "cancel.runtime.stop", &fn);
        ir.CreateCondBr(ir.CreateICmpEQ(flag, ir.getInt32(0)), continuation, abort);
        ir.SetInsertPoint(abort);
        ir.CreateCall(stop);
        ir.CreateUnreachable();
        ++points;
      }
    }
    // Whole native-body extents are eligible only for the frame-abandonment
    // experiment. Ordinary unwinding still requires valid asynchronous CFI.
    const auto size = extents.contains(fn.getName().str()) ? extents.at(fn.getName().str()) : 1;
    auto *end =
        ConstantExpr::getGetElementPtr(Type::getInt8Ty(context), &fn, ConstantInt::get(i64, size));
    regions.push_back(ConstantStruct::get(regionType, {&fn, end}));
  }
  new GlobalVariable(*module, ArrayType::get(regionType, regions.size()), true,
                     GlobalValue::ExternalLinkage,
                     ConstantArray::get(ArrayType::get(regionType, regions.size()), regions),
                     "kalandra_cancel_regions");
  new GlobalVariable(*module, i64, true, GlobalValue::ExternalLinkage,
                     ConstantInt::get(i64, regions.size()), "kalandra_cancel_region_count");
  if (verifyModule(*module, &errs()))
    return 1;
  std::error_code error;
  raw_fd_ostream output(argv[2], error);
  if (error)
    return 1;
  module->print(output, nullptr);
  outs() << mode << ": " << regions.size() << " native regions; " << points
         << " explicit poll sites\n";
}
