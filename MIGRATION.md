# LLVM 20 New Pass Manager Migration Plan

## Context

KLEE's LLVM IR transformation pipeline uses the legacy `llvm::legacy::PassManager` API, which was removed in LLVM 17. The `llvm20` branch has stub files (`Instrument.cpp`, `Optimize.cpp`) with `assert(0)` — no migration work has been done. This plan covers porting all 9 KLEE custom passes to the new PM and rewriting the 3 stubbed orchestration functions.

## Files to modify

- `lib/Module/Passes.h` — Pass class declarations (legacy base → `PassInfoMixin`)
- `lib/Module/Instrument.cpp` — `instrument()` and `checkModule()` implementations
- `lib/Module/Optimize.cpp` — `optimiseAndPrepare()` and `optimizeModule()` implementations
- `lib/Module/RaiseAsm.cpp` — RaiseAsmPass implementation
- `lib/Module/Checks.cpp` — DivCheckPass and OvershiftCheckPass implementations
- `lib/Module/IntrinsicCleaner.cpp` — IntrinsicCleanerPass implementation
- `lib/Module/PhiCleaner.cpp` — PhiCleanerPass implementation
- `lib/Module/LowerSwitch.cpp` — LowerSwitchPass implementation
- `lib/Module/InstructionOperandTypeCheckPass.cpp` — InstructionOperandTypeCheckPass implementation
- `lib/Module/FunctionAlias.cpp` — FunctionAliasPass implementation
- `lib/Module/OptNone.cpp` — OptNonePass implementation
- `lib/Module/ModuleHelper.h` — No changes needed (free functions, not pass-aware)

## Migration pattern (applies to every KLEE custom pass)

The new pass manager uses `llvm::PassInfoMixin<T>` instead of `llvm::ModulePass`/`llvm::FunctionPass`. The migration pattern for each pass is:

**Old (legacy PM):**
```cpp
class MyPass : public llvm::ModulePass {
  static char ID;
public:
  MyPass() : ModulePass(ID) {}
  bool runOnModule(llvm::Module &M) override;
};
```

**New (new PM):**
```cpp
class MyPass : public llvm::PassInfoMixin<MyPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};
```

Key differences:
- No `static char ID`
- No inheritance from `ModulePass`/`FunctionPass`
- `run()` returns `PreservedAnalyses` instead of `bool`
- Analysis manager parameter added
- For function passes: `run(llvm::Function &F, llvm::FunctionAnalysisManager &AM)`

Return value mapping:
- `return false;` (no changes) → `return PreservedAnalyses::all();`
- `return true;` (changed IR) → `return PreservedAnalyses::none();`

---

## Phase 1: OptNonePass (Simplest — warm-up)

**File:** `lib/Module/OptNone.cpp` (59 LOC)
**Complexity:** Very Low
**Type:** Module pass
**Dependencies:** None
**What it does:** Marks functions containing `klee_*` calls with `Attribute::OptimizeNone` + `Attribute::NoInline`

### Steps
1. In `Passes.h`: Change `class OptNonePass : public llvm::ModulePass` → `class OptNonePass : public llvm::PassInfoMixin<OptNonePass>`
2. Remove `static char ID` and constructor
3. In `OptNone.cpp`: Change `bool runOnModule(Module &M)` → `PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM)`
4. Remove `char OptNonePass::ID = 0;`
5. Return `PreservedAnalyses::none()` if modified, `PreservedAnalyses::all()` otherwise

### Verification
- Build KLEE with LLVM 20
- Run: `klee --optimize=true` on a simple test with `klee_make_symbolic` — verify the function containing the call is not optimized away

---

## Phase 2: DivCheckPass + OvershiftCheckPass

**File:** `lib/Module/Checks.cpp` (~123 LOC combined)
**Complexity:** Low
**Type:** Module passes
**Dependencies:** Should run after ScalarizerPass

### DivCheckPass (~55 LOC)
**What it does:** Finds SDiv/UDiv/SRem/URem instructions, injects call to `klee_div_zero_check()` before each (skips constant non-zero divisors). Uses `KleeIRMetaData` annotations to avoid duplicate checks.

#### Steps
1. In `Passes.h`: Change class declaration to `PassInfoMixin<DivCheckPass>`
2. Remove `static char ID` and constructor
3. In `Checks.cpp`: Change signature to `PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM)`
4. Remove `char DivCheckPass::ID = 0;`
5. Return appropriate `PreservedAnalyses`

### OvershiftCheckPass (~68 LOC)
**What it does:** Finds Shl/LShr/AShr instructions, injects call to `klee_overshift_check(bitwidth, shiftAmount)` before each (skips constant shifts in valid range).

#### Steps
Same pattern as DivCheckPass.

### Verification
- Run: `klee --check-div-zero --check-overshift` on test with symbolic divisor/shift amount
- Verify KLEE detects division by zero and overshift errors

---

## Phase 3: PhiCleanerPass

**File:** `lib/Module/PhiCleaner.cpp` (83 LOC)
**Complexity:** Low
**Type:** Function pass
**Dependencies:** Should run after LowerSwitchPass

### What it does
1. Ensures all PHI nodes in a basic block have incoming blocks in the same order
2. Breaks PHI-to-PHI dependencies by inserting `BitCastInst` temporaries

### Steps
1. In `Passes.h`: Change to `PassInfoMixin<PhiCleanerPass>`
2. Remove `static char ID` and constructor
3. In `PhiCleaner.cpp`: Change signature to `PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM)`
4. Move existing `runOnFunction()` logic into `run()`
5. Remove `char PhiCleanerPass::ID = 0;`

### Verification
- Run KLEE on a test with switch statements and PHI nodes
- Verify correct execution with `--switch-type=simple`

---

## Phase 4: RaiseAsmPass

**File:** `lib/Module/RaiseAsm.cpp` (124 LOC)
**Complexity:** Low
**Type:** Module pass
**Dependencies:** None (runs first in pipeline)

### What it does
Raises common glibc inline asm patterns to normal LLVM IR using `TargetLowering` infrastructure. Target-specific (x86/x86_64 on Linux/macOS/FreeBSD).

### Steps
1. In `Passes.h`: Change to `PassInfoMixin<RaiseAsmPass>`
2. Remove `static char ID` and constructor
3. Move `TLI` and `triple` to be initialized in `run()` (they were set in `runOnModule`)
4. In `RaiseAsm.cpp`: Change signature to `PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM)`
5. Remove `char RaiseAsmPass::ID = 0;`

### Verification
- Build and run KLEE on a program that uses glibc functions with inline asm
- Verify no "unsupported inline asm" errors at execution time

---

## Phase 5: LowerSwitchPass

**File:** `lib/Module/LowerSwitch.cpp` (140 LOC)
**Complexity:** Low-Medium
**Type:** Function pass
**Dependencies:** None (but PhiCleanerPass should run after it)

### What it does
Replaces `SwitchInst` with linear chain of `if (val == caseN) goto blockN` branches. Custom implementation (avoids LLVM's binary search which creates extra paths). Reconstructs PHI nodes for successor blocks.

### Steps
1. In `Passes.h`: Change to `PassInfoMixin<LowerSwitchPass>`
2. Remove `static char ID` and constructor
3. Keep `SwitchCase`, `CaseVector`, `CaseItr` types and private methods as-is
4. In `LowerSwitch.cpp`: Change signature to `PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM)`
5. Remove `char LowerSwitchPass::ID = 0;`

### Verification
- Run KLEE with `--switch-type=simple` on a program with switch statements
- Compare test cases generated vs legacy KLEE on same program

---

## Phase 6: FunctionAliasPass

**File:** `lib/Module/FunctionAlias.cpp` (226 LOC)
**Complexity:** Medium
**Type:** Module pass
**Dependencies:** Reads `--function-alias` CLI option

### What it does
Parses `--function-alias=<pattern>:<replacement>` options. Supports exact name match and regex. Type-checks replacement functions (return type, parameters, varargs). Creates `GlobalAlias` to redirect calls.

### Steps
1. In `Passes.h`: Change to `PassInfoMixin<FunctionAliasPass>`
2. Remove `static char ID` and constructor
3. Static helper methods (`getFunctionType`, `checkType`, `tryToReplace`, `isFunctionOrGlobalFunctionAlias`) remain unchanged
4. In `FunctionAlias.cpp`: Change signature to `PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM)`
5. Remove `char FunctionAliasPass::ID = 0;`

### Verification
- Run KLEE with `--function-alias=malloc:my_malloc` on a test program
- Verify the alias is applied and `my_malloc` is called instead

---

## Phase 7: InstructionOperandTypeCheckPass

**File:** `lib/Module/InstructionOperandTypeCheckPass.cpp` (183 LOC)
**Complexity:** Low-Medium
**Type:** Module pass (validation only — never modifies IR)
**Dependencies:** Must run after ScalarizerPass

### What it does
Checks 20+ instruction types to ensure operands are scalar (not vector). Reports violations as warnings. Sets internal `instructionOperandsConform` flag checked by caller after pass runs.

### Special consideration
This pass maintains state (`instructionOperandsConform`) that is checked *after* the pass runs via `checkPassed()`. In the new PM, there are two approaches:
- **Option A:** Keep it as a pass, store result in a member, caller checks after `run()` returns
- **Option B:** Convert to an analysis pass (`AnalysisInfoMixin`) that returns a result object

**Recommended: Option A** — simpler, matches current usage pattern. The caller (`checkModule()`) creates the pass, runs it, then checks the result. With new PM, the caller creates a `ModulePassManager`, adds the pass, runs it, then checks the pass object.

### Steps
1. In `Passes.h`: Change to `PassInfoMixin<InstructionOperandTypeCheckPass>`
2. Keep `instructionOperandsConform` member and `checkPassed()` accessor
3. Remove `static char ID`, update constructor
4. In implementation: Change to `PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM)`
5. Always return `PreservedAnalyses::all()` (never modifies IR)

### Verification
- Compile a test with vector instructions, verify KLEE rejects it with "Unexpected instruction operand types"
- Compile a normal scalar test, verify it passes validation

---

## Phase 8: IntrinsicCleanerPass

**File:** `lib/Module/IntrinsicCleaner.cpp` (447 LOC)
**Complexity:** High (most complex pass)
**Type:** Module pass
**Dependencies:** Requires `DataLayout` at construction

### What it does
Handles 40+ LLVM intrinsic types:
- `llvm.va_copy`, `llvm.va_end` — varargs
- `llvm.dbg.*` — debug info (removed)
- `llvm.{s,u}{add,sub,mul}.with.overflow.*` — overflow arithmetic (expanded to explicit checks)
- `llvm.{s,u}add.sat.*`, `llvm.{s,u}sub.sat.*` — saturating arithmetic
- `llvm.trap`, `llvm.objectsize` — special handling
- `freeze` instructions — replaced with null/identity
- Uses `IRBuilder` extensively for IR reconstruction

### Steps
1. In `Passes.h`: Change to `PassInfoMixin<IntrinsicCleanerPass>`
2. Keep `DataLayout` member and `IntrinsicLowering` member
3. Constructor takes `const DataLayout &TD` (same as before, just remove `ModulePass(ID)`)
4. In `IntrinsicCleaner.cpp`: Change `runOnModule` to `PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM)`
5. Internal helpers (`runOnBasicBlock`, `runOnFunction`) remain unchanged — they don't use PM APIs
6. Remove `char IntrinsicCleanerPass::ID = 0;`

### Verification
- Run KLEE on a program using `__builtin_add_overflow`, `__builtin_trap`, varargs
- Verify correct symbolic execution through these constructs

---

## Phase 9: Wire up `instrument()` and `checkModule()`

**File:** `lib/Module/Instrument.cpp`
**Dependencies:** Phases 1-8 (all custom passes ported)

### What it does
Replaces the `assert(0)` stubs with real implementations using new PM.

### `instrument()` implementation
```cpp
// Set up analysis managers
PassBuilder PB;
LoopAnalysisManager LAM;
FunctionAnalysisManager FAM;
CGSCCAnalysisManager CGAM;
ModuleAnalysisManager MAM;
PB.registerModuleAnalyses(MAM);
PB.registerCGSCCAnalyses(CGAM);
PB.registerFunctionAnalyses(FAM);
PB.registerLoopAnalyses(LAM);
PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

ModulePassManager MPM;
FunctionPassManager FPM;

MPM.addPass(RaiseAsmPass());
FPM.addPass(ScalarizerPass());           // LLVM built-in (new PM ready)
FPM.addPass(LowerAtomicPass());          // LLVM built-in (new PM ready)
MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
if (CheckDivZero)  MPM.addPass(DivCheckPass());
if (CheckOvershift) MPM.addPass(OvershiftCheckPass());
MPM.addPass(IntrinsicCleanerPass(module->getDataLayout()));
MPM.run(*module, MAM);
```

### `checkModule()` implementation
```cpp
// Run verifier via pass manager
PassBuilder PB;
LoopAnalysisManager LAM;
FunctionAnalysisManager FAM;
CGSCCAnalysisManager CGAM;
ModuleAnalysisManager MAM;
PB.registerModuleAnalyses(MAM);
PB.registerCGSCCAnalyses(CGAM);
PB.registerFunctionAnalyses(FAM);
PB.registerLoopAnalyses(LAM);
PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

ModulePassManager MPM;
if (!DontVerify) MPM.addPass(VerifierPass());
MPM.run(*module, MAM);

// Run type check pass directly (needs post-run state inspection)
InstructionOperandTypeCheckPass typeCheck;
typeCheck.run(*module, MAM);
if (!typeCheck.checkPassed()) {
  klee_error("Unexpected instruction operand types detected");
}
```

### Steps
1. Add includes for new PM headers (`llvm/IR/PassManager.h`, `llvm/Passes/PassBuilder.h`, `llvm/Transforms/Scalar/Scalarizer.h`, `llvm/Transforms/Utils/LowerAtomic.h`)
2. Set up analysis managers with `PassBuilder` (boilerplate above)
3. Implement `instrument()` as above
4. Implement `checkModule()` — run verifier via PM, run type check pass directly

### Verification
- Run KLEE end-to-end on a simple symbolic test
- Verify instrumentation checks fire (div-by-zero, overshift)
- Verify module validation passes

---

## Phase 10: Wire up `optimiseAndPrepare()` and `optimizeModule()`

**File:** `lib/Module/Optimize.cpp`
**Dependencies:** Phase 9 complete

### What it does
This is the largest piece — the full optimization pipeline.

### `optimiseAndPrepare()` implementation

Three sub-pipelines, matching the legacy structure:

**Sub-pipeline 1:** OptNonePass (conditional)
```cpp
if (!OptimiseKLEECall) {
  ModulePassManager MPM;
  MPM.addPass(OptNonePass());
  MPM.run(*module, MAM);
}
```

**Sub-pipeline 2:** Optimization (conditional)
```cpp
if (Optimize)
  optimizeModule(module, preservedFunctions);
```

**Sub-pipeline 3:** Final preparation
```cpp
ModulePassManager MPM;
FunctionPassManager FPM;
FPM.addPass(SimplifyCFGPass());
switch (SwitchType) {
case SwitchImplType::eSwitchTypeInternal:
  break;
case SwitchImplType::eSwitchTypeSimple:
  FPM.addPass(klee::LowerSwitchPass());   // KLEE custom
  break;
case SwitchImplType::eSwitchTypeLLVM:
  FPM.addPass(llvm::LowerSwitchPass());   // LLVM built-in
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
```

### `optimizeModule()` implementation

**Recommended approach:** Instead of manually replicating 40+ passes, use LLVM's `PassBuilder` to construct a standard `O2` pipeline:

```cpp
PassBuilder PB;
ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
// Add KLEE-specific passes (internalize, etc.) around it
MPM.run(*module, MAM);
```

This is simpler, forward-compatible, and gives equivalent results. The legacy code was essentially a hand-rolled O2 pipeline.

**If exact pass-for-pass replication is needed**, map each `create*Pass()` call to its new PM equivalent:

| Legacy | New PM |
|--------|--------|
| `createCFGSimplificationPass()` | `SimplifyCFGPass()` |
| `createPromoteMemoryToRegisterPass()` | `PromotePass()` |
| `createGlobalOptimizerPass()` | `GlobalOptPass()` |
| `createGlobalDCEPass()` | `GlobalDCEPass()` |
| `createSCCPPass()` | `SCCPPass()` |
| `createDeadArgEliminationPass()` | `DeadArgumentEliminationPass()` |
| `createInstructionCombiningPass()` | `InstCombinePass()` |
| `createFunctionInliningPass()` | `ModuleInlinerWrapperPass()` |
| `createJumpThreadingPass()` | `JumpThreadingPass()` |
| `createSROAPass()` | `SROAPass(SROAOptions::ModifyCFG)` |
| `createTailCallEliminationPass()` | `TailCallElimPass()` |
| `createReassociatePass()` | `ReassociatePass()` |
| `createLoopRotatePass()` | `LoopRotatePass()` |
| `createLICMPass()` | `LICMPass()` |
| `createIndVarSimplifyPass()` | `IndVarSimplifyPass()` |
| `createLoopDeletionPass()` | `LoopDeletionPass()` |
| `createLoopUnrollPass()` | `LoopUnrollPass()` |
| `createGVNPass()` | `GVNPass()` |
| `createMemCpyOptPass()` | `MemCpyOptPass()` |
| `createDeadStoreEliminationPass()` | `DSEPass()` |
| `createAggressiveDCEPass()` | `ADCEPass()` |
| `createStripDeadPrototypesPass()` | `StripDeadPrototypesPass()` |
| `createConstantMergePass()` | `ConstantMergePass()` |
| `createInternalizePass(fn)` | `InternalizePass(fn)` |
| `createIPSCCPPass()` | `IPSCCPPass()` |
| `createPostOrderFunctionAttrsLegacyPass()` | `PostOrderFunctionAttrsPass()` |
| `createReversePostOrderFunctionAttrsPass()` | `ReversePostOrderFunctionAttrsPass()` |
| `createStripSymbolsPass(bool)` | `StripSymbolsPass(bool)` |
| `createVerifierPass()` | `VerifierPass()` |

**Removed passes** (no longer exist in LLVM 20, safe to drop):
- `createPruneEHPass()` — was already gated to LLVM <= 15
- `createArgumentPromotionPass()` — was already gated to LLVM <= 14
- `createLoopUnswitchPass()` — was already gated to LLVM <= 14

**Note on FunctionPass vs ModulePass in new PM:**
Function passes must be wrapped with `createModuleToFunctionPassAdaptor()` when added to a `ModulePassManager`. Loop passes need `createFunctionToLoopPassAdaptor()`.

### Verification
- Run KLEE with `--optimize` on a realistic program
- Compare generated test cases with KLEE built against LLVM 16 (legacy PM)
- Verify code coverage is comparable

---

## Phase 11: Remove version gates and clean up

### Steps
1. Remove `#if LLVM_VERSION_CODE <= LLVM_VERSION(15, 0)` blocks (PruneEH)
2. Remove `#if LLVM_VERSION_CODE <= LLVM_VERSION(14, 0)` blocks (ArgumentPromotion, LoopUnswitch)
3. Remove legacy PM includes (`llvm/IR/LegacyPassManager.h`, `llvm/Pass.h`)
4. Remove `InstrumentLegacy.cpp` and `OptimizeLegacy.cpp` from build (or keep for LLVM < 17 support)
5. Update `CMakeLists.txt` if the version split is no longer needed
6. Remove `DISABLE_WARNING_DEPRECATED_DECLARATIONS` guards that were for legacy PM

### Verification
- Full KLEE test suite: `make check` / `lit test/`
- Unit tests: `make unittests`

---

## Platform testing notes: Apple Silicon macOS

The pass manager migration is purely an LLVM API change — pass logic stays identical. All passes
operate on LLVM IR which is architecture-independent. Apple Silicon macOS is a viable development
and test platform for this migration with the following caveats.

### Build configuration for macOS ARM64

```bash
cmake -DLLVM_DIR=/path/to/llvm20/lib/cmake/llvm \
      -DENABLE_SOLVER_Z3=ON \
      -DENABLE_POSIX_RUNTIME=OFF \
      -DENABLE_KLEE_UCLIBC=OFF \
      ..
```

uclibc is Linux-only; POSIX runtime can be enabled but is not required for pass testing.

### Per-phase testability on Apple Silicon

| Phase | Pass | Apple Silicon? | Notes |
|-------|------|----------------|-------|
| 1 | OptNonePass | Yes | Pure attribute marking |
| 2 | DivCheckPass + OvershiftCheckPass | Yes | IR instrumentation |
| 3 | PhiCleanerPass | Yes | PHI node manipulation |
| 4 | RaiseAsmPass | **Partial** | x86/x86_64 only — runs as no-op on ARM64; verify build + no crash, but can't test actual asm raising |
| 5 | LowerSwitchPass | Yes | CFG transformation |
| 6 | FunctionAliasPass | Yes | Global value manipulation |
| 7 | InstructionOperandTypeCheckPass | Yes | Type validation |
| 8 | IntrinsicCleanerPass | Yes | Intrinsic lowering |
| 9 | Wire `instrument()` + `checkModule()` | Yes | Pipeline orchestration |
| 10 | Wire `optimiseAndPrepare()` + `optimizeModule()` | Yes | Pipeline orchestration |
| 11 | Full test suite | **Partial** | ~10 tests skipped on darwin (see below) |

### Tests excluded on darwin (skipped automatically by lit)

These tests carry `REQUIRES: not-darwin` and will be skipped:
- **Variadic function tests** (6 tests in `test/VarArgs/`) — hardcoded x86-64 ABI in `Executor.cpp`
- **Inline assembly tests** (3 tests) — `Feature/InlineAsm.c`, `Feature/RaiseAsm.c`, `Feature/asm_lifting.ll`
- **Misc tests** (~10 total) — `LargeReturnTypes`, `Memalign`, `MemoryLimit`, some alias tests

### Recommendation

- Use Apple Silicon macOS for day-to-day development and testing of all phases
- For **Phase 4 (RaiseAsmPass)**, verify on x86_64 Linux (CI or remote machine) to test actual asm raising
- Before merging each phase, run the full test suite on Linux x86_64 for complete coverage
- The existing GitHub Actions CI includes a macOS job — extend it for LLVM 20 when ready

---

## Summary: Suggested weekly schedule

| Week | Phase | Pass(es) | LOC | Difficulty |
|------|-------|----------|-----|------------|
| 1 | Phase 1 | OptNonePass | 59 | Very Low |
| 2 | Phase 2 | DivCheckPass + OvershiftCheckPass | 123 | Low |
| 3 | Phase 3 | PhiCleanerPass | 83 | Low |
| 4 | Phase 4 | RaiseAsmPass | 124 | Low |
| 5 | Phase 5 | LowerSwitchPass | 140 | Low-Medium |
| 6 | Phase 6 | FunctionAliasPass | 226 | Medium |
| 7 | Phase 7 | InstructionOperandTypeCheckPass | 183 | Low-Medium |
| 8 | Phase 8 | IntrinsicCleanerPass | 447 | High |
| 9 | Phase 9 | Wire `instrument()` + `checkModule()` | ~80 | Medium |
| 10 | Phase 10 | Wire `optimiseAndPrepare()` + `optimizeModule()` | ~250 | High |
| 11 | Phase 11 | Cleanup + version gates + test suite | — | Low |
