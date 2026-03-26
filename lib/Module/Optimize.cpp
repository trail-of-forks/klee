//===-- Optimize.cpp - Optimize a complete program -------------------------===//
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
#include "klee/Support/OptionCategories.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/IPO/ConstantMerge.h"
#include "llvm/Transforms/IPO/DeadArgumentElimination.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/IPO/GlobalOpt.h"
#include "llvm/Transforms/IPO/Internalize.h"
#include "llvm/Transforms/IPO/StripDeadPrototypes.h"
#include "llvm/Transforms/IPO/StripSymbols.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"

using namespace llvm;
using namespace klee;

namespace {

static cl::opt<bool>
    DisableInline("disable-inlining",
                  cl::desc("Do not run the inliner pass (default=false)"),
                  cl::init(false), cl::cat(klee::ModuleCat));

static cl::opt<bool> DisableInternalize(
    "disable-internalize",
    cl::desc("Do not mark all symbols as internal (default=false)"),
    cl::init(false), cl::cat(klee::ModuleCat));

static cl::opt<bool> VerifyEach("verify-each",
                                cl::desc("Verify intermediate results of all "
                                         "optimization passes (default=false)"),
                                cl::init(false), cl::cat(klee::ModuleCat));

static cl::opt<bool>
    Strip("strip-all",
          cl::desc("Strip all symbol information from executable "
                   "(default=false)"),
          cl::init(false), cl::cat(klee::ModuleCat));

static cl::opt<bool>
    StripDebug("strip-debug",
               cl::desc("Strip debugger symbol info from executable "
                        "(default=false)"),
               cl::init(false), cl::cat(klee::ModuleCat));

} // namespace

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

/// Optimize - Perform link time optimizations. This will run the scalar
/// optimizations, any loaded plugin-optimization modules, and then the
/// inter-procedural optimizations if applicable.
void klee::optimizeModule(llvm::Module *M,
                          llvm::ArrayRef<const char *> preservedFunctions) {
  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  setupAnalysisManagers(PB, LAM, FAM, CGAM, MAM);

  // Use LLVM's standard O2 pipeline instead of hand-rolling 40+ passes.
  // The legacy code was essentially a hand-built O2 pipeline.
  ModulePassManager MPM =
      PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);

  // Now that composite has been compiled, scan through the module, looking
  // for a main function.  If main is defined, mark all other functions
  // internal.
  if (!DisableInternalize) {
    auto PreserveFunctions = [=](const GlobalValue &GV) {
      StringRef GVName = GV.getName();
      for (const char *fun : preservedFunctions)
        if (GVName == fun)
          return true;
      return false;
    };
    MPM.addPass(InternalizePass(PreserveFunctions));
  }

  // Post-internalization cleanup
  MPM.addPass(GlobalOptPass());
  MPM.addPass(GlobalDCEPass());
  MPM.addPass(ConstantMergePass());
  MPM.addPass(DeadArgumentEliminationPass());

  // Clean up after IPO passes
  MPM.addPass(createModuleToFunctionPassAdaptor(InstCombinePass()));
  MPM.addPass(createModuleToFunctionPassAdaptor(SimplifyCFGPass()));
  MPM.addPass(GlobalDCEPass());

  // Strip symbols if requested.
  // In the legacy PM, createStripSymbolsPass(true) meant "only strip debug".
  // In the new PM, StripSymbolsPass strips all, StripNonDebugSymbolsPass
  // strips non-debug only, and StripDebugDeclarePass strips debug info.
  if (Strip)
    MPM.addPass(StripSymbolsPass());
  else if (StripDebug)
    MPM.addPass(StripDebugDeclarePass());

  // Final cleanup
  MPM.addPass(createModuleToFunctionPassAdaptor(InstCombinePass()));
  MPM.addPass(createModuleToFunctionPassAdaptor(SimplifyCFGPass()));
  MPM.addPass(createModuleToFunctionPassAdaptor(ADCEPass()));
  MPM.addPass(GlobalDCEPass());

  MPM.run(*M, MAM);
}

void klee::optimiseAndPrepare(bool OptimiseKLEECall, bool Optimize,
                              SwitchImplType SwitchType,
                              std::string EntryPoint,
                              llvm::ArrayRef<const char *> preservedFunctions,
                              llvm::Module *module) {
  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  setupAnalysisManagers(PB, LAM, FAM, CGAM, MAM);

  // Preserve all functions containing klee-related function calls from being
  // optimised around
  if (!OptimiseKLEECall) {
    ModulePassManager MPM;
    MPM.addPass(OptNonePass());
    MPM.run(*module, MAM);
  }

  if (Optimize)
    optimizeModule(module, preservedFunctions);

  // Needs to happen after linking (since ctors/dtors can be modified)
  // and optimization (since global optimization can rewrite lists).
  injectStaticConstructorsAndDestructors(module, EntryPoint);

  // Finally, run the passes that maintain invariants we expect during
  // interpretation. We run the intrinsic cleaner just in case we
  // linked in something with intrinsics but any external calls are
  // going to be unresolved. We really need to handle the intrinsics
  // directly I think?
  ModulePassManager MPM;

  FunctionPassManager FPM;
  FPM.addPass(SimplifyCFGPass());
  switch (SwitchType) {
  case SwitchImplType::eSwitchTypeInternal:
    break;
  case SwitchImplType::eSwitchTypeSimple:
    FPM.addPass(klee::LowerSwitchPass());
    break;
  case SwitchImplType::eSwitchTypeLLVM:
    FPM.addPass(llvm::LowerSwitchPass());
    break;
  }
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));

  MPM.addPass(IntrinsicCleanerPass(module->getDataLayout()));

  FunctionPassManager FPM2;
  FPM2.addPass(ScalarizerPass());
  FPM2.addPass(PhiCleanerPass());
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM2)));

  MPM.addPass(FunctionAliasPass());
  MPM.run(*module, MAM);
}
