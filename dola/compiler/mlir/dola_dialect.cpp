#include "dola/dola_dialect.h"
#include "dola/runtime_operations.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

#include <utility>

using namespace mlir;

#include "compiler/mlir/dola_dialect.cpp.inc"

#define GET_TYPEDEF_CLASSES
#include "compiler/mlir/dola_types.cpp.inc"

#define GET_OP_CLASSES
#include "compiler/mlir/dola_ops.cpp.inc"

namespace dola::ir {

namespace {

bool isRuntimeBacked(mlir::Type type) {
  return mlir::isa<StringType, RecordType, EnumType, TupleType, OptionType,
                   ResultType, ListType, MapType, SenderType, ReceiverType,
                   TaskType, ListenerType, ConnectionType>(type);
}

} // namespace

void DolaDialect::initialize() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "compiler/mlir/dola_types.cpp.inc"
      >();
  addOperations<
#define GET_OP_LIST
#include "compiler/mlir/dola_ops.cpp.inc"
      >();
}

mlir::LogicalResult
TupleType::verify(llvm::function_ref<mlir::InFlightDiagnostic()> emitError,
                  llvm::ArrayRef<mlir::Type> elementTypes) {
  if (elementTypes.size() < 2 || elementTypes.size() > 8)
    return emitError() << "tuple type requires 2 to 8 elements";
  return mlir::success();
}

mlir::LogicalResult RuntimeOp::verify() {
  const auto operation = parseRuntimeOperation(getCallee());
  if (!operation)
    return emitOpError() << "has unknown runtime callee `" << getCallee()
                         << "`";
  const auto& descriptor = describeRuntimeOperation(*operation);
  const int inputs = static_cast<int>(getInputs().size());
  if (inputs < descriptor.minimumInputs ||
      (descriptor.maximumInputs >= 0 && inputs > descriptor.maximumInputs))
    return emitOpError() << "has an invalid input count for `" << getCallee()
                         << "`";
  if (std::cmp_not_equal(getNumResults(), descriptor.results))
    return emitOpError() << "`" << getCallee() << "` requires "
                         << descriptor.results << " results but has "
                         << getNumResults();
  if (std::cmp_not_equal(getMetadata().size(), descriptor.metadata))
    return emitOpError() << "requires " << descriptor.metadata
                         << " metadata values";
  switch (*operation) {
  case RuntimeOperation::StringConcat:
    if (!llvm::all_of(getInputs().getTypes(),
                      [](mlir::Type type) { return mlir::isa<StringType>(type); }) ||
        !mlir::isa<StringType>(getResult(0).getType()))
      return emitOpError("`string.concat` requires String inputs and result");
    break;
  case RuntimeOperation::IntToString:
    if (!getInputs().front().getType().isInteger(64) ||
        !mlir::isa<StringType>(getResult(0).getType()))
      return emitOpError("`int.to_string` requires an Int input and String result");
    break;
  default:
    break;
  }
  return mlir::success();
}

mlir::LogicalResult TaskSpawnOp::verify() {
  auto function = getOperation()
                      ->getParentOfType<mlir::ModuleOp>()
                      .lookupSymbol<mlir::func::FuncOp>(getCallee());
  if (!function)
    return emitOpError("references an unknown function");
  if (function.getArgumentTypes().empty() ||
      !mlir::isa<ContextType>(function.getArgumentTypes().front()) ||
      function.getArgumentTypes().drop_front() != getInputs().getTypes())
    return emitOpError("requires inputs matching the callee parameters");
  if (function.getResultTypes().empty() ||
      !function.getResultTypes().front().isInteger(32) ||
      function.getNumResults() > 2)
    return emitOpError("requires a status-returning Dola callee");
  mlir::Type result = function.getNumResults() == 2
                          ? function.getResultTypes()[1]
                          : mlir::Type{};
  const auto taskResult = getResult().getType().getElementType();
  if ((result && result != taskResult) ||
      (!result && !mlir::isa<UnitType>(taskResult)))
    return emitOpError("requires a task result matching the callee result");
  return mlir::success();
}

mlir::LogicalResult ValueEqualOp::verify() {
  return getLeft().getType() == getRight().getType()
             ? mlir::success()
             : emitOpError("requires operands with the same type");
}

mlir::LogicalResult RetainOp::verify() {
  if (getValue().getType() != getResult().getType())
    return emitOpError("requires matching input and result types");
  return isRuntimeBacked(getValue().getType())
             ? mlir::success()
             : emitOpError("requires a runtime-backed value");
}

mlir::LogicalResult ReleaseOp::verify() {
  return isRuntimeBacked(getValue().getType())
             ? mlir::success()
             : emitOpError("requires a runtime-backed value");
}

mlir::LogicalResult SlotAllocOp::verify() {
  return getInitial().getType() == getSlot().getType().getElementType()
             ? mlir::success()
             : emitOpError("requires the initial value to match the slot type");
}

mlir::LogicalResult SlotLoadOp::verify() {
  return getValue().getType() == getSlot().getType().getElementType()
             ? mlir::success()
             : emitOpError("requires the result to match the slot type");
}

mlir::LogicalResult SlotStoreOp::verify() {
  return getValue().getType() == getSlot().getType().getElementType()
             ? mlir::success()
             : emitOpError("requires the value to match the slot type");
}

} // namespace dola::ir
