#include "dola/dola_dialect.h"
#include "dola/lowering.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"
#include "llvm/Support/InitLLVM.h"

int main(int argc, char** argv) {
  llvm::InitLLVM initialization(argc, argv);
  dola::registerDolaLoweringPass();
  mlir::DialectRegistry registry;
  registry.insert<dola::ir::DolaDialect, mlir::arith::ArithDialect,
                  mlir::cf::ControlFlowDialect, mlir::func::FuncDialect,
                  mlir::LLVM::LLVMDialect, mlir::memref::MemRefDialect,
                  mlir::scf::SCFDialect>();
  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "Dola MLIR optimizer\n", registry));
}
