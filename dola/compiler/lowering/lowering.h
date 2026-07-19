#ifndef DOLA_LOWERING_LOWERING_H
#define DOLA_LOWERING_LOWERING_H

#include "mlir/IR/BuiltinOps.h"

#include <filesystem>
#include <memory>
#include <string>

namespace llvm {
class Module;
}
namespace mlir {
class MLIRContext;
class Pass;
}

namespace dola {
enum class EntryResultKind { Unit, Int, Result };
std::unique_ptr<mlir::Pass> createLowerToLlvmPass();
void registerDolaLoweringPass();
bool lowerToLlvmDialect(mlir::ModuleOp module, std::string& error);
std::unique_ptr<llvm::Module> translateToLlvmIr(mlir::ModuleOp module,
                                                mlir::MLIRContext& context,
                                                const std::string& entrySymbol,
                                                EntryResultKind entryResult,
                                                std::string& error);
bool emitNativeObject(llvm::Module& module, const std::filesystem::path& path,
                      std::string& error);
} // namespace dola
#endif
