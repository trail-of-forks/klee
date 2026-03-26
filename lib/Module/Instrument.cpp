//===-- Instrument.cpp ------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ModuleHelper.h"

#include "Passes.h"
#include "klee/Config/Version.h"
#include "klee/Support/ErrorHandling.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Scalar/LowerAtomicPass.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"

using namespace llvm;
using namespace klee;

/// Set up the full analysis manager stack needed by the new pass manager.
static void setupAnalysisManagers(PassBuilder &PB, LoopAnalysisManager &LAM,
                                  FunctionAnalysisManager &FAM,
                                  CGSCCAnalysisManager &CGAM,
                                  ModuleAnalysisManager &MAM) {
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
}

void klee::instrument(bool CheckDivZero, bool CheckOvershift,
                      llvm::Module *module) {
  // Inject checks prior to optimization... we also perform the
  // invariant transformations that we will end up doing later so that
  // optimize is seeing what is as close as possible to the final
  // module.
  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  setupAnalysisManagers(PB, LAM, FAM, CGAM, MAM);

  ModulePassManager MPM;
  MPM.addPass(RaiseAsmPass());

  // This pass will scalarize as much code as possible so that the Executor
  // does not need to handle operands of vector type for most instructions
  // other than InsertElementInst and ExtractElementInst.
  //
  // NOTE: Must come before division/overshift checks because those passes
  // don't know how to handle vector instructions.
  FunctionPassManager FPM;
  FPM.addPass(ScalarizerPass());

  // This pass will replace atomic instructions with non-atomic operations
  FPM.addPass(LowerAtomicPass());
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));

  if (CheckDivZero)
    MPM.addPass(DivCheckPass());
  if (CheckOvershift)
    MPM.addPass(OvershiftCheckPass());

  MPM.addPass(IntrinsicCleanerPass(module->getDataLayout()));
  MPM.run(*module, MAM);
}

void klee::checkModule(bool DontVerfify, llvm::Module *module) {
  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  setupAnalysisManagers(PB, LAM, FAM, CGAM, MAM);

  // Run verifier if requested
  if (!DontVerfify) {
    ModulePassManager MPM;
    MPM.addPass(VerifierPass());
    MPM.run(*module, MAM);
  }

  // Run type check pass directly so we can inspect its post-run state.
  // Enforce the operand type invariants that the Executor expects. This
  // implicitly depends on the "Scalarizer" pass to be run in order to succeed
  // in the presence of vector instructions.
  InstructionOperandTypeCheckPass typeCheck;
  typeCheck.run(*module, MAM);
  if (!typeCheck.checkPassed()) {
    klee_error("Unexpected instruction operand types detected");
  }
}
