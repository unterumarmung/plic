#ifndef DOLA_MLIR_MLIR_GEN_H
#define DOLA_MLIR_MLIR_GEN_H

#include "dola/ast.h"
#include "dola/sema.h"

#include "mlir/IR/BuiltinOps.h"

#include <memory>
#include <vector>

namespace mlir {
class MLIRContext;
}

namespace dola {
mlir::OwningOpRef<mlir::ModuleOp>
generateMlir(mlir::MLIRContext& context, const std::vector<Module>& modules,
             const SemanticResult& semantics);
std::string mangleFunction(const Module& module, const FunctionDecl& function);
} // namespace dola
#endif
