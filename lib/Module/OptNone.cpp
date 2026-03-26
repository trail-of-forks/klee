//===-- OptNone.cpp -------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Passes.h"

#include "klee/Config/Version.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"

using namespace llvm;

static bool runOptNonePass(Module &M) {
  // Find list of functions that start with `klee_`
  // and mark all functions that contain such call or invoke as optnone
  SmallPtrSet<Function *, 16> CallingFunctions;
  for (auto &F : M) {
    if (!F.hasName())
      continue;
#if LLVM_VERSION_CODE >= LLVM_VERSION(16, 0)
    if (!F.getName().starts_with("klee_"))
      continue;
#else
    if (!F.getName().startswith("klee_"))
      continue;
#endif

    for (auto *U : F.users()) {
      // skip non-calls and non-invokes
      if (!isa<CallInst>(U) && !isa<InvokeInst>(U))
        continue;
      auto *Inst = cast<Instruction>(U);
      CallingFunctions.insert(Inst->getParent()->getParent());
    }
  }

  bool changed = false;
  for (auto F : CallingFunctions) {
    // Skip if already annotated
    if (F->hasFnAttribute(Attribute::OptimizeNone))
      continue;
    F->addFnAttr(Attribute::OptimizeNone);
    F->addFnAttr(Attribute::NoInline);
    changed = true;
  }

  return changed;
}

namespace klee {

#if LLVM_VERSION_MAJOR >= 17
PreservedAnalyses OptNonePass::run(Module &M, ModuleAnalysisManager &AM) {
  if (runOptNonePass(M))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
#else
char OptNonePass::ID;

bool OptNonePass::runOnModule(Module &M) { return runOptNonePass(M); }
#endif

} // namespace klee
