//===-- PhiCleaner.cpp ----------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Passes.h"

#include "klee/Config/Version.h"

#include "llvm/IR/IRBuilder.h"

#include <set>

using namespace llvm;

static bool runPhiCleanerPass(Function &f) {
  bool changed = false;

  for (Function::iterator b = f.begin(), be = f.end(); b != be; ++b) {
    BasicBlock::iterator it = b->begin();

    if (it->getOpcode() == Instruction::PHI) {
      PHINode *reference = cast<PHINode>(it);

      std::set<Value *> phis;
      phis.insert(reference);

      unsigned numBlocks = reference->getNumIncomingValues();
      for (++it; isa<PHINode>(*it); ++it) {
        PHINode *pi = cast<PHINode>(it);

        assert(numBlocks == pi->getNumIncomingValues());

        // see if it is out of order
        unsigned i;
        for (i = 0; i < numBlocks; i++)
          if (pi->getIncomingBlock(i) != reference->getIncomingBlock(i))
            break;

        if (i != numBlocks) {
          std::vector<Value *> values;
          values.reserve(numBlocks);
          for (unsigned i = 0; i < numBlocks; i++)
            values.push_back(
                pi->getIncomingValueForBlock(reference->getIncomingBlock(i)));
          for (unsigned i = 0; i < numBlocks; i++) {
            pi->setIncomingBlock(i, reference->getIncomingBlock(i));
            pi->setIncomingValue(i, values[i]);
          }
          changed = true;
        }

        // see if it uses any previously defined phi nodes
        for (i = 0; i < numBlocks; i++) {
          Value *value = pi->getIncomingValue(i);

          if (phis.find(value) != phis.end()) {
            // fix by making a "move" at the end of the incoming block
            // to a new temporary, which is thus known not to be a phi
            // result. we could be somewhat more efficient about this
            // by sharing temps and by reordering phi instructions so
            // this isn't completely necessary, but in the end this is
            // just a pathological case which does not occur very
            // often.
            IRBuilder<> Builder(pi->getIncomingBlock(i)->getTerminator());
            auto *tmp = Builder.CreateBitCast(
                value, value->getType(), value->getName() + ".phiclean");
            pi->setIncomingValue(i, tmp);
            changed = true;
          }
        }

        phis.insert(pi);
      }
    }
  }

  return changed;
}

namespace klee {

#if LLVM_VERSION_MAJOR >= 17
PreservedAnalyses PhiCleanerPass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
  if (runPhiCleanerPass(F))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
#else
char PhiCleanerPass::ID = 0;

bool PhiCleanerPass::runOnFunction(Function &f) {
  return runPhiCleanerPass(f);
}
#endif

} // namespace klee
