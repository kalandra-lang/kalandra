#pragma once
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include <map>
#include <utility>

namespace kalandra {
// Keep array layout fields in scalar SSA across source-level binding swaps.
// An aggregate PHI hides equal lengths/strides behind a cyclic descriptor,
// preventing LLVM from proving their invariance in an otherwise scalar loop.
// This changes neither the descriptor representation nor any access checks.
inline bool scalarizeNativeArrayPhis(llvm::Module &module) {
  using namespace llvm;
  bool changed = false;
  for (auto &function : module) {
    if (function.isDeclaration() || function.hasFnAttribute("kalandra.task.frame"))
      continue;
    struct Split {
      PHINode *original;
      SmallVector<SmallVector<unsigned, 2>, 16> paths;
      SmallVector<PHINode *, 16> fields;
    };
    SmallVector<Split, 8> splits;
    std::map<PHINode *, unsigned> indices;
    for (auto &block : function)
      for (auto &phi : block.phis()) {
        auto *type = dyn_cast<StructType>(phi.getType());
        if (!type || type->isOpaque() || type->getNumElements() != 5 ||
            !type->getElementType(0)->isPointerTy() ||
            !type->getElementType(1)->isPointerTy() ||
            !type->getElementType(2)->isIntegerTy(64))
          continue;
        auto *sizes = dyn_cast<ArrayType>(type->getElementType(3));
        auto *strides = dyn_cast<ArrayType>(type->getElementType(4));
        // Bound the rewrite's code growth; larger descriptors retain their
        // existing lowering and all functions still pass stack admission.
        if (!sizes || !strides || sizes != strides ||
            !sizes->getElementType()->isIntegerTy(64) || sizes->getNumElements() > 16)
          continue;
        bool available = true;
        for (unsigned n = 0; n < phi.getNumIncomingValues(); ++n)
          available &= phi.getIncomingValue(n) != phi.getIncomingBlock(n)->getTerminator();
        if (!available)
          continue;
        Split split{&phi, {}, {}};
        split.paths = {{0}, {1}, {2}};
        for (unsigned field : {3U, 4U})
          for (unsigned n = 0; n < sizes->getNumElements(); ++n)
            split.paths.push_back({field, n});
        IRBuilder<> ir(&phi);
        for (const auto &path : split.paths)
          split.fields.push_back(ir.CreatePHI(ExtractValueInst::getIndexedType(type, path),
                                              phi.getNumIncomingValues(), "array.field"));
        indices.emplace(&phi, splits.size());
        splits.push_back(std::move(split));
      }
    // Fill every scalar PHI before erasing any original. Mutually swapped
    // descriptors then refer directly to one another's corresponding fields.
    for (auto &split : splits) {
      auto *phi = split.original;
      for (unsigned n = 0; n < phi->getNumIncomingValues(); ++n) {
        auto *incoming = phi->getIncomingValue(n);
        auto *predecessor = phi->getIncomingBlock(n);
        const auto other = indices.find(dyn_cast<PHINode>(incoming));
        IRBuilder<> at(predecessor->getTerminator());
        for (unsigned field = 0; field < split.fields.size(); ++field) {
          auto *value = other == indices.end()
                            ? at.CreateExtractValue(incoming, split.paths[field])
                            : splits[other->second].fields[field];
          split.fields[field]->addIncoming(value, predecessor);
        }
      }
    }
    for (auto &split : splits) {
      auto *phi = split.original;
      IRBuilder<> at(&*phi->getParent()->getFirstInsertionPt());
      Value *descriptor = PoisonValue::get(phi->getType());
      for (unsigned field = 0; field < split.fields.size(); ++field)
        descriptor = at.CreateInsertValue(descriptor, split.fields[field], split.paths[field]);
      phi->replaceAllUsesWith(descriptor);
    }
    for (auto &split : splits)
      split.original->eraseFromParent();
    changed |= !splits.empty();
  }
  return changed;
}
} // namespace kalandra
