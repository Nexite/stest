// stest MMIO interception pass.
//
// Rewrites every volatile scalar load/store (integer, float, pointer; <= 64 bits)
// into a call of the stest runtime ABI:
//   uintN_t __stest_mmio_loadN (const volatile void *a);
//   void    __stest_mmio_storeN(volatile void *a, uintN_t v);      N in {8,16,32,64}
// Non-integer values are bit-cast to/from the integer of the same width.
// Volatile atomic accesses are left untouched and reported as an optimization remark.
//
// Registration choice: the pass is registered ONLY at registerPipelineStartEPCallback.
// That callback runs exactly once per module at every optimization level, including
// -O0 (buildO0DefaultPipeline invokes it), and it runs before any optimization could
// merge, split or delete volatile accesses. Registering also at OptimizerLast would
// run it twice at -O2 (and gains nothing), so it is not. As a belt-and-braces guard
// the pass records a "stest.mmio.instrumented" named-metadata marker and skips
// modules that already carry it, so it stays idempotent if scheduled again.
//
// Written against the LLVM 18 API (opaque pointers).

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Passes/PassBuilder.h"
#if __has_include("llvm/Plugins/PassPlugin.h")
#include "llvm/Plugins/PassPlugin.h" // LLVM >= 20
#else
#include "llvm/Passes/PassPlugin.h"  // LLVM 18/19
#endif

using namespace llvm;

namespace {

constexpr const char *kMarker = "stest.mmio.instrumented";
constexpr const char *kRemarkPass = "stest-mmio";

struct StestMmioPass : PassInfoMixin<StestMmioPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    if (M.getNamedMetadata(kMarker))
      return PreservedAnalyses::all();
    M.getOrInsertNamedMetadata(kMarker);

    bool Changed = false;
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      Changed |= runOnFunction(F);
    }
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  // Run even for -O0 functions that carry optnone.
  static bool isRequired() { return true; }

private:
  // Returns the integer bit width (8/16/32/64) used for the ABI call, or 0 if the
  // type is not supported.
  static unsigned abiWidth(Type *Ty, const DataLayout &DL) {
    if (!(Ty->isIntegerTy() || Ty->isFloatingPointTy() || Ty->isPointerTy()))
      return 0;
    if (Ty->isX86_FP80Ty() || Ty->isFP128Ty() || Ty->isPPC_FP128Ty())
      return 0;
    if (Ty->isIntegerTy() && !isPowerOf2_32(Ty->getIntegerBitWidth()))
      return 0; // i1, i24, ... not representable
    TypeSize Sz = DL.getTypeSizeInBits(Ty);
    if (Sz.isScalable())
      return 0;
    uint64_t W = Sz.getFixedValue();
    return (W == 8 || W == 16 || W == 32 || W == 64) ? unsigned(W) : 0;
  }

  // i8/i16 values must be zero-extended by the caller (C ABI of uint8_t/uint16_t
  // parameters and returns), so mark them zeroext on declaration and call sites.
  static AttributeList loadAttrs(LLVMContext &C, unsigned W) {
    if (W >= 32)
      return AttributeList();
    return AttributeList::get(C, AttributeList::ReturnIndex,
                              {Attribute::ZExt});
  }
  static AttributeList storeAttrs(LLVMContext &C, unsigned W) {
    if (W >= 32)
      return AttributeList();
    return AttributeList::get(C, AttributeList::FirstArgIndex + 1,
                              {Attribute::ZExt});
  }

  static CallInst *callLoad(IRBuilder<> &B, Module &M, unsigned W, Value *Ptr) {
    LLVMContext &C = M.getContext();
    FunctionCallee F = M.getOrInsertFunction(
        ("__stest_mmio_load" + Twine(W)).str(), loadAttrs(C, W),
        Type::getIntNTy(C, W), PointerType::getUnqual(C));
    CallInst *CI = B.CreateCall(F, {Ptr});
    CI->setAttributes(loadAttrs(C, W));
    return CI;
  }
  static CallInst *callStore(IRBuilder<> &B, Module &M, unsigned W, Value *Ptr,
                             Value *V) {
    LLVMContext &C = M.getContext();
    FunctionCallee F = M.getOrInsertFunction(
        ("__stest_mmio_store" + Twine(W)).str(), storeAttrs(C, W),
        Type::getVoidTy(C), PointerType::getUnqual(C), Type::getIntNTy(C, W));
    CallInst *CI = B.CreateCall(F, {Ptr, V});
    CI->setAttributes(storeAttrs(C, W));
    return CI;
  }

  static void remark(OptimizationRemarkEmitter &ORE, Instruction &I, StringRef Why) {
    ORE.emit([&] {
      return OptimizationRemarkMissed(kRemarkPass, "VolatileNotInstrumented", &I)
             << "volatile access not routed to stest MMIO: " << Why.str();
    });
  }

  bool runOnFunction(Function &F) {
    Module &M = *F.getParent();
    const DataLayout &DL = M.getDataLayout();
    OptimizationRemarkEmitter ORE(&F);

    SmallVector<Instruction *, 16> Work;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
        if (auto *L = dyn_cast<LoadInst>(&I)) {
          if (L->isVolatile())
            Work.push_back(L);
        } else if (auto *S = dyn_cast<StoreInst>(&I)) {
          if (S->isVolatile())
            Work.push_back(S);
        }
      }

    bool Changed = false;
    for (Instruction *I : Work) {
      Value *Ptr;
      Type *ValTy;
      bool Atomic;
      if (auto *L = dyn_cast<LoadInst>(I)) {
        Ptr = L->getPointerOperand();
        ValTy = L->getType();
        Atomic = L->isAtomic();
      } else {
        auto *S = cast<StoreInst>(I);
        Ptr = S->getPointerOperand();
        ValTy = S->getValueOperand()->getType();
        Atomic = S->isAtomic();
      }
      if (Atomic) {
        remark(ORE, *I, "atomic volatile access left untouched");
        continue;
      }
      unsigned W = abiWidth(ValTy, DL);
      if (!W || Ptr->getType()->getPointerAddressSpace() != 0) {
        remark(ORE, *I, "unsupported type or address space");
        continue;
      }

      IRBuilder<> B(I);
      LLVMContext &C = M.getContext();
      Type *IntTy = Type::getIntNTy(C, W);
      if (auto *L = dyn_cast<LoadInst>(I)) {
        Value *V = callLoad(B, M, W, Ptr);
        if (ValTy->isPointerTy())
          V = B.CreateIntToPtr(V, ValTy);
        else if (!ValTy->isIntegerTy())
          V = B.CreateBitCast(V, ValTy);
        L->replaceAllUsesWith(V);
      } else {
        Value *V = cast<StoreInst>(I)->getValueOperand();
        if (ValTy->isPointerTy())
          V = B.CreatePtrToInt(V, IntTy);
        else if (!ValTy->isIntegerTy())
          V = B.CreateBitCast(V, IntTy);
        callStore(B, M, W, Ptr, V);
      }
      I->eraseFromParent();
      Changed = true;
    }
    return Changed;
  }
};

} // namespace

extern "C" __attribute__((visibility("default"))) PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "StestMmioPass", "0.1", [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "stest-mmio") {
                    MPM.addPass(StestMmioPass());
                    return true;
                  }
                  return false;
                });
            PB.registerPipelineStartEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(StestMmioPass());
                });
          }};
}
