#pragma once
#include <vector>
namespace llvm {
class GlobalVariable;
class Module;
} // namespace llvm
namespace kalandra::continuation {
struct ModulePlan;
}
namespace kalandra {
// Private checked-plan component linkage. Kernel entries must already be linked
// into the checked LLVM module. Uses the production frame/construct/resume/result
// emitter without a CLI root wrapper. This does not perform source admission.
std::vector<llvm::GlobalVariable *> emitTaskEntries(continuation::ModulePlan &, llvm::Module &);
} // namespace kalandra
