#ifndef DOLA_MLIR_DOLA_DIALECT_H
#define DOLA_MLIR_DOLA_DIALECT_H

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/Types.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "compiler/mlir/dola_dialect.h.inc"

#define GET_TYPEDEF_CLASSES
#include "compiler/mlir/dola_types.h.inc"

#define GET_OP_CLASSES
#include "compiler/mlir/dola_ops.h.inc"

#endif
