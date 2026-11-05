// Instrument optimized project loads, without changing BwTree's access paths or
// allocation layout. The backend itself and external libraries are not instrumented.
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
using namespace llvm;
namespace {
struct LocalNccPass : PassInfoMixin<LocalNccPass> {
  PreservedAnalyses run(Module& module, ModuleAnalysisManager&) {
    auto& context = module.getContext();
    auto* length_type = module.getDataLayout().getIntPtrType(context);
    auto hook = module.getOrInsertFunction("local_ncc_read", Type::getVoidTy(context),
                                          PointerType::getUnqual(context), length_type);
    auto store_hook = module.getOrInsertFunction("local_ncc_write", Type::getVoidTy(context),
                                          PointerType::getUnqual(context), length_type);
    auto prefetch_hook = module.getOrInsertFunction("local_ncc_prefetch", Type::getVoidTy(context),
                                          PointerType::getUnqual(context));
    auto atomic_hook = module.getOrInsertFunction("local_ncc_atomic", Type::getVoidTy(context),
                                          PointerType::getUnqual(context), length_type);
    for (auto& function : module) {
      if (function.isDeclaration() || function.getName().startswith("local_ncc_")) continue;
      SmallVector<Instruction*, 64> original;
      for (auto& block : function) for (auto& instruction : block) original.push_back(&instruction);
      for (auto* instruction : original) {
        IRBuilder<> builder(instruction);
        auto access = [&](FunctionCallee callee, Value* pointer, Value* length) {
          builder.CreateCall(callee, {builder.CreatePointerCast(pointer, PointerType::getUnqual(context)),
                                   builder.CreateZExtOrTrunc(length, length_type)});
        };
        auto read = [&](Value* pointer, Value* length) { access(hook, pointer, length); };
        auto write = [&](Value* pointer, Value* length) { access(store_hook, pointer, length); };
        auto size = [&](Type* type) {
          const auto bytes = module.getDataLayout().getTypeStoreSize(type);
          if (bytes.isScalable()) report_fatal_error("LOCAL_NCC: scalable vector access unsupported");
          return ConstantInt::get(length_type, bytes.getFixedValue());
        };
        if (auto* load = dyn_cast<LoadInst>(instruction)) {
          if (load->isAtomic()) access(atomic_hook, load->getPointerOperand(), size(load->getType()));
          read(load->getPointerOperand(), size(load->getType()));
        } else if (auto* store = dyn_cast<StoreInst>(instruction)) {
          if (store->isAtomic()) access(atomic_hook, store->getPointerOperand(), size(store->getValueOperand()->getType()));
          write(store->getPointerOperand(), size(store->getValueOperand()->getType()));
        } else if (auto* rmw = dyn_cast<AtomicRMWInst>(instruction)) {
          access(atomic_hook, rmw->getPointerOperand(), size(rmw->getValOperand()->getType()));
        } else if (auto* cas = dyn_cast<AtomicCmpXchgInst>(instruction)) {
          access(atomic_hook, cas->getPointerOperand(), size(cas->getCompareOperand()->getType()));
        } else if (auto* transfer = dyn_cast<MemTransferInst>(instruction)) {
          read(transfer->getSource(), transfer->getLength());
          write(transfer->getDest(), transfer->getLength());
        } else if (auto* set = dyn_cast<MemSetInst>(instruction)) {
          write(set->getDest(), set->getLength());
        } else if (auto* call = dyn_cast<CallInst>(instruction)) {
          auto* callee = call->getCalledFunction();
          if (!callee) continue;
          const auto name = callee->getName();
          if (callee->getIntrinsicID() == Intrinsic::prefetch) {
            builder.CreateCall(prefetch_hook, {call->getArgOperand(0)});
          } else if ((name == "memcmp" || name == "bcmp") && call->arg_size() == 3) {
            read(call->getArgOperand(0), call->getArgOperand(2));
            read(call->getArgOperand(1), call->getArgOperand(2));
          } else if ((name == "memcpy" || name == "memmove") && call->arg_size() == 3) {
            read(call->getArgOperand(1), call->getArgOperand(2));
            write(call->getArgOperand(0), call->getArgOperand(2));
          } else if (name == "memset" && call->arg_size() == 3) {
            write(call->getArgOperand(0), call->getArgOperand(2));
          }
        }
      }
    }
    return PreservedAnalyses::none();
  }
};
}
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "local-ncc-loads", LLVM_VERSION_STRING,
    [](PassBuilder& builder) {
      builder.registerOptimizerLastEPCallback([](ModulePassManager& manager, OptimizationLevel) {
        manager.addPass(LocalNccPass());
      });
    }};
}
