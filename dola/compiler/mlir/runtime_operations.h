#ifndef DOLA_MLIR_RUNTIME_OPERATIONS_H
#define DOLA_MLIR_RUNTIME_OPERATIONS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace dola::ir {

enum class RuntimeOperation : std::uint32_t {
#define DOLA_CODEGEN_OPERATION(NAME, C_NAME, VALUE, MLIR_NAME, MIN_INPUTS,      \
                               MAX_INPUTS, RESULTS, METADATA)                  \
  NAME = VALUE,
#include "runtime/codegen_operations.def"
#undef DOLA_CODEGEN_OPERATION
};

struct RuntimeOperationDescriptor {
  RuntimeOperation operation;
  llvm::StringLiteral name;
  int minimumInputs;
  int maximumInputs;
  int results;
  int metadata;
};

std::optional<RuntimeOperation> parseRuntimeOperation(llvm::StringRef name);
const RuntimeOperationDescriptor& describeRuntimeOperation(
    RuntimeOperation operation);
llvm::ArrayRef<RuntimeOperationDescriptor> runtimeOperationDescriptors();

} // namespace dola::ir

#endif
