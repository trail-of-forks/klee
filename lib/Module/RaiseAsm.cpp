//===-- RaiseAsm.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Passes.h"
#include "klee/Config/Version.h"
#include "klee/Support/ErrorHandling.h"

#include <memory>

#include "klee/Support/CompilerWarning.h"
DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED_DECLARATIONS
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(16, 0)
#include "llvm/TargetParser/Host.h"
#else
#include "llvm/Support/Host.h"
#endif
#if LLVM_VERSION_CODE >= LLVM_VERSION(14, 0)
#include "llvm/MC/TargetRegistry.h"
#else
#include "llvm/Support/TargetRegistry.h"
#endif
#include "llvm/Target/TargetMachine.h"
DISABLE_WARNING_POP

using namespace llvm;
using namespace klee;

Function *RaiseAsmPass::getIntrinsic(Module &M, unsigned IID, Type **Tys,
                                     unsigned NumTys) {
  return Intrinsic::getDeclaration(&M, (Intrinsic::ID)IID,
                                   ArrayRef<Type *>(Tys, NumTys));
}

// FIXME: This should just be implemented as a patch to
// X86TargetAsmInfo.cpp, then everyone will benefit.
bool RaiseAsmPass::runOnInstruction(Module &M, Instruction *I) {
  // We can just raise inline assembler using calls
  CallInst *ci = dyn_cast<CallInst>(I);
  if (!ci)
    return false;

  InlineAsm *ia = dyn_cast<InlineAsm>(ci->getCalledOperand());
  if (!ia)
    return false;

  // Try to use existing infrastructure
  if (!TLI)
    return false;

  if (TLI->ExpandInlineAsm(ci))
    return true;

  if ((triple.getArch() == Triple::x86 ||
       triple.getArch() == Triple::x86_64) &&
      (triple.isOSLinux() || triple.isMacOSX() || triple.isOSFreeBSD())) {

    if (ia->getAsmString() == "" && ia->hasSideEffects() &&
        ia->getFunctionType()->getReturnType()->isVoidTy()) {
      IRBuilder<> Builder(I);
      Builder.CreateFence(AtomicOrdering::SequentiallyConsistent);
      I->eraseFromParent();
      return true;
    }
  }

  return false;
}

bool RaiseAsmPass::runOnModule(Module &M) {
  bool changed = false;
  std::string Err;

  // Use target triple from the module if possible.
  std::string TargetTriple = M.getTargetTriple();
  if (TargetTriple.empty())
    TargetTriple = sys::getDefaultTargetTriple();
  const Target *Tgt = TargetRegistry::lookupTarget(TargetTriple, Err);

  std::unique_ptr<TargetMachine> TM;
  TLI = nullptr;
  if (Tgt == nullptr) {
    klee_warning("Warning: unable to select target: %s", Err.c_str());
  } else {
#if LLVM_VERSION_CODE >= LLVM_VERSION(16, 0)
    TM.reset(Tgt->createTargetMachine(TargetTriple, "", "", TargetOptions(),
                                      std::nullopt));
#else
    TM.reset(Tgt->createTargetMachine(TargetTriple, "", "", TargetOptions(),
                                      None));
#endif
    if (TM && !M.empty()) {
      if (auto *STI = TM->getSubtargetImpl(*(M.begin())))
        TLI = STI->getTargetLowering();
      if (TLI)
        triple = Triple(TargetTriple);
    }
  }

  for (Module::iterator fi = M.begin(), fe = M.end(); fi != fe; ++fi) {
    for (Function::iterator bi = fi->begin(), be = fi->end(); bi != be; ++bi) {
      for (BasicBlock::iterator ii = bi->begin(), ie = bi->end(); ii != ie;) {
        Instruction *i = &*ii;
        ++ii;
        changed |= runOnInstruction(M, i);
      }
    }
  }

  return changed;
}

#if LLVM_VERSION_MAJOR >= 17
PreservedAnalyses RaiseAsmPass::run(Module &M, ModuleAnalysisManager &AM) {
  if (runOnModule(M))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
#else
char RaiseAsmPass::ID = 0;
#endif
