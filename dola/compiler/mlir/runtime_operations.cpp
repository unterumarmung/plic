#include "dola/runtime_operations.h"

#include "llvm/ADT/ArrayRef.h"

#include <array>

namespace dola::ir {
namespace {

constexpr std::array descriptors = {
#define DOLA_CODEGEN_OPERATION(NAME, C_NAME, VALUE, MLIR_NAME, MIN_INPUTS,      \
                               MAX_INPUTS, RESULTS, METADATA)                  \
  RuntimeOperationDescriptor{RuntimeOperation::NAME, MLIR_NAME, MIN_INPUTS,    \
                             MAX_INPUTS, RESULTS, METADATA},
#include "runtime/codegen_operations.def"
#undef DOLA_CODEGEN_OPERATION
};

#define DOLA_CODEGEN_OPERATION(NAME, C_NAME, VALUE, MLIR_NAME, MIN_INPUTS,      \
                               MAX_INPUTS, RESULTS, METADATA)                  \
  static_assert(static_cast<std::uint32_t>(RuntimeOperation::NAME) == (VALUE));
#include "runtime/codegen_operations.def"
#undef DOLA_CODEGEN_OPERATION

} // namespace

std::optional<RuntimeOperation> parseRuntimeOperation(llvm::StringRef name) {
  for (const auto& descriptor : descriptors)
    if (descriptor.name == name)
      return descriptor.operation;
  return std::nullopt;
}

const RuntimeOperationDescriptor&
describeRuntimeOperation(RuntimeOperation operation) {
  return descriptors.at(static_cast<std::size_t>(operation));
}

llvm::ArrayRef<RuntimeOperationDescriptor> runtimeOperationDescriptors() {
  return descriptors;
}

} // namespace dola::ir
