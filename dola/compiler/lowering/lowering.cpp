#include "dola/lowering.h"

#include "dola/dola_dialect.h"
#include "dola/runtime_operations.h"

#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/LLVMCommon/ConversionTarget.h"
#include "mlir/Conversion/LLVMCommon/Pattern.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Transforms/DialectConversion.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <system_error>

namespace dola {
namespace {

mlir::LLVM::LLVMStructType valueType(mlir::MLIRContext* context) {
  return mlir::LLVM::LLVMStructType::getLiteral(
      context,
      {mlir::IntegerType::get(context, 32), mlir::IntegerType::get(context, 32),
       mlir::IntegerType::get(context, 64)});
}

mlir::Value constant(mlir::ConversionPatternRewriter& rewriter,
                     mlir::Location location, int64_t value, unsigned bits) {
  return mlir::LLVM::ConstantOp::create(rewriter, location,
                                        rewriter.getIntegerType(bits), value);
}

mlir::Value packValue(mlir::ConversionPatternRewriter& rewriter,
                      mlir::Location location, mlir::Value input) {
  auto type = valueType(rewriter.getContext());
  if (input.getType() == type)
    return input;
  uint32_t tag = input.getType().isInteger(1) ? 1
                 : input.getType().isF64()    ? 3
                                              : 2;
  mlir::Value bits = input;
  if (input.getType().isInteger(1))
    bits = mlir::LLVM::ZExtOp::create(rewriter, location, rewriter.getI64Type(),
                                      input);
  else if (input.getType().isF64())
    bits = mlir::LLVM::BitcastOp::create(rewriter, location,
                                         rewriter.getI64Type(), input);
  auto result = mlir::LLVM::UndefOp::create(rewriter, location, type);
  auto tagged = mlir::LLVM::InsertValueOp::create(
      rewriter, location, result, constant(rewriter, location, tag, 32),
      llvm::ArrayRef<int64_t>{0});
  auto reserved = mlir::LLVM::InsertValueOp::create(
      rewriter, location, tagged, constant(rewriter, location, 0, 32),
      llvm::ArrayRef<int64_t>{1});
  return mlir::LLVM::InsertValueOp::create(rewriter, location, reserved, bits,
                                           llvm::ArrayRef<int64_t>{2});
}

mlir::Value packKnownValue(mlir::ConversionPatternRewriter& rewriter,
                           mlir::Location location, mlir::Value input,
                           mlir::Type inputType) {
  auto type = valueType(rewriter.getContext());
  if (inputType == type)
    return input;
  const uint32_t tag = inputType.isInteger(1) ? 1 : inputType.isF64() ? 3 : 2;
  mlir::Value bits = input;
  if (inputType.isInteger(1))
    bits = mlir::LLVM::ZExtOp::create(rewriter, location, rewriter.getI64Type(),
                                      input);
  else if (inputType.isF64())
    bits = mlir::LLVM::BitcastOp::create(rewriter, location,
                                         rewriter.getI64Type(), input);
  auto result = mlir::LLVM::UndefOp::create(rewriter, location, type);
  auto tagged = mlir::LLVM::InsertValueOp::create(
      rewriter, location, result, constant(rewriter, location, tag, 32),
      llvm::ArrayRef<int64_t>{0});
  auto reserved = mlir::LLVM::InsertValueOp::create(
      rewriter, location, tagged, constant(rewriter, location, 0, 32),
      llvm::ArrayRef<int64_t>{1});
  return mlir::LLVM::InsertValueOp::create(rewriter, location, reserved, bits,
                                           llvm::ArrayRef<int64_t>{2});
}

mlir::Value unitValue(mlir::ConversionPatternRewriter& rewriter,
                      mlir::Location location) {
  auto type = valueType(rewriter.getContext());
  auto result = mlir::LLVM::UndefOp::create(rewriter, location, type);
  auto tagged = mlir::LLVM::InsertValueOp::create(
      rewriter, location, result, constant(rewriter, location, 0, 32),
      llvm::ArrayRef<int64_t>{0});
  auto reserved = mlir::LLVM::InsertValueOp::create(
      rewriter, location, tagged, constant(rewriter, location, 0, 32),
      llvm::ArrayRef<int64_t>{1});
  return mlir::LLVM::InsertValueOp::create(rewriter, location, reserved,
                                           constant(rewriter, location, 0, 64),
                                           llvm::ArrayRef<int64_t>{2});
}

mlir::Value unpackValue(mlir::ConversionPatternRewriter& rewriter,
                        mlir::Location location, mlir::Value value,
                        mlir::Type resultType) {
  if (resultType == value.getType())
    return value;
  auto bits = mlir::LLVM::ExtractValueOp::create(rewriter, location,
                                                 rewriter.getI64Type(), value,
                                                 llvm::ArrayRef<int64_t>{2});
  if (resultType.isInteger(1))
    return mlir::LLVM::TruncOp::create(rewriter, location, resultType, bits);
  if (resultType.isF64())
    return mlir::LLVM::BitcastOp::create(rewriter, location, resultType, bits);
  return bits;
}

mlir::Value addressOfValue(mlir::ConversionPatternRewriter& rewriter,
                           mlir::Location location, mlir::Value value) {
  auto pointer = mlir::LLVM::LLVMPointerType::get(rewriter.getContext());
  auto one = constant(rewriter, location, 1, 64);
  auto storage = mlir::LLVM::AllocaOp::create(
      rewriter, location, pointer, valueType(rewriter.getContext()), one, 8);
  mlir::LLVM::StoreOp::create(rewriter, location, value, storage);
  return {storage};
}

class RuntimeCallBuilder {
public:
  explicit RuntimeCallBuilder(mlir::ConversionPatternRewriter& rewriter)
      : rewriter_(&rewriter) {}

  mlir::LLVM::LLVMFuncOp getOrInsert(mlir::Operation* anchor,
                                     llvm::StringRef name,
                                     mlir::LLVM::LLVMFunctionType type) {
    auto module = anchor->getParentOfType<mlir::ModuleOp>();
    if (auto function = module.lookupSymbol<mlir::LLVM::LLVMFuncOp>(name))
      return function;
    mlir::OpBuilder::InsertionGuard guard(*rewriter_);
    rewriter_->setInsertionPointToStart(module.getBody());
    return mlir::LLVM::LLVMFuncOp::create(*rewriter_, anchor->getLoc(), name,
                                          type);
  }

  std::pair<mlir::Value, mlir::Value> emitBytes(mlir::Operation* anchor,
                                                llvm::StringRef bytes) {
    auto module = anchor->getParentOfType<mlir::ModuleOp>();
    auto* context = rewriter_->getContext();
    auto i8 = rewriter_->getI8Type();
    auto arrayType = mlir::LLVM::LLVMArrayType::get(i8, bytes.size());
    const std::string name =
        "__dola_string_" + std::to_string(nextGlobal_.fetch_add(1));
    mlir::LLVM::GlobalOp global;
    {
      mlir::OpBuilder::InsertionGuard guard(*rewriter_);
      rewriter_->setInsertionPointToStart(module.getBody());
      global = mlir::LLVM::GlobalOp::create(
          *rewriter_, anchor->getLoc(), arrayType, true,
          mlir::LLVM::Linkage::Internal, name, rewriter_->getStringAttr(bytes));
    }
    auto pointerType = mlir::LLVM::LLVMPointerType::get(context);
    auto address = mlir::LLVM::AddressOfOp::create(
        *rewriter_, anchor->getLoc(), pointerType, global.getSymNameAttr());
    auto pointer = mlir::LLVM::GEPOp::create(
        *rewriter_, anchor->getLoc(), pointerType, arrayType, address,
        llvm::ArrayRef<mlir::LLVM::GEPArg>{0, 0});
    auto length = mlir::LLVM::ConstantOp::create(
        *rewriter_, anchor->getLoc(), rewriter_->getI64Type(),
        static_cast<int64_t>(bytes.size()));
    return {pointer, length};
  }

private:
  mlir::ConversionPatternRewriter* rewriter_;
  static std::atomic_uint64_t nextGlobal_;
};

std::atomic_uint64_t RuntimeCallBuilder::nextGlobal_{0};

struct StringLiteralLowering
    : mlir::ConvertOpToLLVMPattern<ir::StringLiteralOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  mlir::LogicalResult
  matchAndRewrite(ir::StringLiteralOp operation, OpAdaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    RuntimeCallBuilder runtime(rewriter);
    auto [pointer, length] = runtime.emitBytes(operation, operation.getValue());
    auto pointerType = mlir::LLVM::LLVMPointerType::get(rewriter.getContext());
    auto one = constant(rewriter, operation.getLoc(), 1, 64);
    auto out =
        mlir::LLVM::AllocaOp::create(rewriter, operation.getLoc(), pointerType,
                                     valueType(rewriter.getContext()), one, 8);
    auto functionType = mlir::LLVM::LLVMFunctionType::get(
        rewriter.getI32Type(),
        {pointerType, rewriter.getI64Type(), pointerType});
    auto function =
        runtime.getOrInsert(operation, "dola_rt_string_create", functionType);
    mlir::LLVM::CallOp::create(rewriter, operation.getLoc(), function,
                               mlir::ValueRange{pointer, length, out});
    rewriter.replaceOpWithNewOp<mlir::LLVM::LoadOp>(
        operation, valueType(rewriter.getContext()), out);
    return mlir::success();
  }
};

template <typename Operation>
struct OutputLowering : mlir::ConvertOpToLLVMPattern<Operation> {
  OutputLowering(const mlir::LLVMTypeConverter& converter, llvm::StringRef name)
      : mlir::ConvertOpToLLVMPattern<Operation>(converter), name_(name) {}

  mlir::LogicalResult
  matchAndRewrite(Operation operation, Operation::Adaptor adaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    auto pointer =
        addressOfValue(rewriter, operation.getLoc(), adaptor.getValue());
    auto functionType = mlir::LLVM::LLVMFunctionType::get(
        rewriter.getI32Type(), {pointer.getType(), rewriter.getI8Type()});
    RuntimeCallBuilder runtime(rewriter);
    auto function =
        runtime.getOrInsert(operation, "dola_rt_codegen_print", functionType);
    mlir::LLVM::CallOp::create(
        rewriter, operation.getLoc(), function,
        mlir::ValueRange{pointer,
                         constant(rewriter, operation.getLoc(),
                                  name_ == "dola_rt_println_value", 8)});
    rewriter.eraseOp(operation);
    return mlir::success();
  }

private:
  std::string name_;
};

struct PanicLowering : mlir::ConvertOpToLLVMPattern<ir::PanicOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  mlir::LogicalResult
  matchAndRewrite(ir::PanicOp operation, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    RuntimeCallBuilder runtime(rewriter);
    auto message =
        addressOfValue(rewriter, operation.getLoc(), adaptor.getMessage());
    auto functionType = mlir::LLVM::LLVMFunctionType::get(
        rewriter.getI32Type(),
        {adaptor.getContext().getType(), message.getType()});
    auto function =
        runtime.getOrInsert(operation, "dola_rt_codegen_context_set_panic",
                            functionType);
    mlir::LLVM::CallOp::create(rewriter, operation.getLoc(), function,
                               mlir::ValueRange{adaptor.getContext(),
                                                message});
    rewriter.eraseOp(operation);
    return mlir::success();
  }
};

struct PoisonLowering : mlir::ConvertOpToLLVMPattern<ir::PoisonOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  mlir::LogicalResult
  matchAndRewrite(ir::PoisonOp operation, OpAdaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    auto converted = getTypeConverter()->convertType(operation.getType());
    rewriter.replaceOpWithNewOp<mlir::LLVM::UndefOp>(operation, converted);
    return mlir::success();
  }
};

struct RetainLowering : mlir::ConvertOpToLLVMPattern<ir::RetainOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  mlir::LogicalResult
  matchAndRewrite(ir::RetainOp operation, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    auto functionType = mlir::LLVM::LLVMFunctionType::get(
        mlir::LLVM::LLVMVoidType::get(rewriter.getContext()),
        {mlir::LLVM::LLVMPointerType::get(rewriter.getContext())});
    RuntimeCallBuilder runtime(rewriter);
    auto function =
        runtime.getOrInsert(operation, "dola_rt_codegen_retain", functionType);
    mlir::LLVM::CallOp::create(
        rewriter, operation.getLoc(), function,
        mlir::ValueRange{
            addressOfValue(rewriter, operation.getLoc(), adaptor.getValue())});
    rewriter.replaceOp(operation, adaptor.getValue());
    return mlir::success();
  }
};

struct ReleaseLowering : mlir::ConvertOpToLLVMPattern<ir::ReleaseOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  mlir::LogicalResult
  matchAndRewrite(ir::ReleaseOp operation, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    auto functionType = mlir::LLVM::LLVMFunctionType::get(
        mlir::LLVM::LLVMVoidType::get(rewriter.getContext()),
        {mlir::LLVM::LLVMPointerType::get(rewriter.getContext())});
    RuntimeCallBuilder runtime(rewriter);
    auto function =
        runtime.getOrInsert(operation, "dola_rt_codegen_release", functionType);
    mlir::LLVM::CallOp::create(
        rewriter, operation.getLoc(), function,
        mlir::ValueRange{
            addressOfValue(rewriter, operation.getLoc(), adaptor.getValue())});
    rewriter.eraseOp(operation);
    return mlir::success();
  }
};

struct EqualLowering : mlir::ConvertOpToLLVMPattern<ir::ValueEqualOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;
  mlir::LogicalResult
  matchAndRewrite(ir::ValueEqualOp operation, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    auto pointer = mlir::LLVM::LLVMPointerType::get(rewriter.getContext());
    auto one = constant(rewriter, operation.getLoc(), 1, 64);
    auto out = mlir::LLVM::AllocaOp::create(
        rewriter, operation.getLoc(), pointer, rewriter.getI8Type(), one, 1);
    auto functionType = mlir::LLVM::LLVMFunctionType::get(
        rewriter.getI32Type(), {pointer, pointer, pointer});
    RuntimeCallBuilder runtime(rewriter);
    auto function =
        runtime.getOrInsert(operation, "dola_rt_codegen_equal", functionType);
    mlir::LLVM::CallOp::create(
        rewriter, operation.getLoc(), function,
        mlir::ValueRange{
            addressOfValue(rewriter, operation.getLoc(), adaptor.getLeft()),
            addressOfValue(rewriter, operation.getLoc(), adaptor.getRight()),
            out});
    auto byte = mlir::LLVM::LoadOp::create(rewriter, operation.getLoc(),
                                           rewriter.getI8Type(), out);
    rewriter.replaceOpWithNewOp<mlir::LLVM::TruncOp>(
        operation, rewriter.getI1Type(), byte);
    return mlir::success();
  }
};

struct TaskSpawnLowering : mlir::ConvertOpToLLVMPattern<ir::TaskSpawnOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  mlir::LogicalResult
  matchAndRewrite(ir::TaskSpawnOp operation, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    auto pointer = mlir::LLVM::LLVMPointerType::get(rewriter.getContext());
    const auto rawValue = valueType(rewriter.getContext());
    const auto location = operation.getLoc();
    RuntimeCallBuilder runtime(rewriter);
    const auto tupleCreateType = mlir::LLVM::LLVMFunctionType::get(
        rewriter.getI32Type(),
        {rewriter.getI64Type(), pointer, rewriter.getI64Type(), pointer});
    auto tupleCreate =
        runtime.getOrInsert(operation, "dola_rt_tuple_create", tupleCreateType);
    const int64_t inputCount = static_cast<int64_t>(adaptor.getInputs().size());
    auto storageCount =
        constant(rewriter, location, inputCount == 0 ? 1 : inputCount, 64);
    auto inputStorage = mlir::LLVM::AllocaOp::create(
        rewriter, location, pointer, rawValue, storageCount, 8);
    for (int64_t index = 0; index < inputCount; ++index) {
      auto slot = mlir::LLVM::GEPOp::create(
          rewriter, location, pointer, rawValue, inputStorage,
          llvm::ArrayRef<mlir::LLVM::GEPArg>{static_cast<int32_t>(index)});
      mlir::LLVM::StoreOp::create(
          rewriter, location,
          packValue(rewriter, location, adaptor.getInputs()[index]), slot);
    }
    auto environmentStorage =
        mlir::LLVM::AllocaOp::create(rewriter, location, pointer, rawValue,
                                     constant(rewriter, location, 1, 64), 8);
    mlir::LLVM::CallOp::create(
        rewriter, location, tupleCreate,
        mlir::ValueRange{constant(rewriter, location, 0, 64), inputStorage,
                         constant(rewriter, location, inputCount, 64),
                         environmentStorage});
    auto environment = mlir::LLVM::LoadOp::create(rewriter, location, rawValue,
                                                  environmentStorage);

    const std::string trampolineName =
        "__dola_task_entry_" + std::to_string(nextTrampoline_.fetch_add(1));
    auto trampolineType = mlir::LLVM::LLVMFunctionType::get(
        rewriter.getI32Type(), {pointer, pointer, pointer});
    mlir::LLVM::LLVMFuncOp trampoline;
    {
      mlir::OpBuilder::InsertionGuard guard(rewriter);
      auto module = operation->getParentOfType<mlir::ModuleOp>();
      rewriter.setInsertionPointToStart(module.getBody());
      trampoline = mlir::LLVM::LLVMFuncOp::create(
          rewriter, location, trampolineName, trampolineType);
      auto* block = new mlir::Block();
      trampoline.getBody().push_back(block);
      block->addArguments({pointer, pointer, pointer},
                          {location, location, location});
      rewriter.setInsertionPointToStart(block);

      llvm::SmallVector<mlir::Value> arguments;
      llvm::SmallVector<mlir::Value> rawArguments;
      auto tupleGetType = mlir::LLVM::LLVMFunctionType::get(
          rewriter.getI32Type(), {pointer, rewriter.getI64Type(), pointer});
      auto tupleGet = runtime.getOrInsert(
          trampoline, "dola_rt_codegen_tuple_get", tupleGetType);
      for (std::size_t index = 0; index < operation.getInputs().size();
           ++index) {
        auto output = mlir::LLVM::AllocaOp::create(
            rewriter, location, pointer, rawValue,
            constant(rewriter, location, 1, 64), 8);
        mlir::LLVM::CallOp::create(
            rewriter, location, tupleGet,
            mlir::ValueRange{
                block->getArgument(1),
                constant(rewriter, location, static_cast<int64_t>(index), 64),
                output});
        auto raw =
            mlir::LLVM::LoadOp::create(rewriter, location, rawValue, output);
        rawArguments.push_back(raw);
        auto converted = getTypeConverter()->convertType(
            operation.getInputs()[index].getType());
        arguments.push_back(unpackValue(rewriter, location, raw, converted));
      }
      auto resultType = operation.getResult().getType().getElementType();
      auto convertedResult = getTypeConverter()->convertType(resultType);
      mlir::Value result;
      mlir::Value taskStatus;
      arguments.insert(arguments.begin(), block->getArgument(0));
      if (mlir::isa<ir::UnitType>(resultType)) {
        auto call = mlir::LLVM::CallOp::create(
            rewriter, location, mlir::TypeRange{rewriter.getI32Type()},
            operation.getCalleeAttr(), arguments);
        taskStatus = call.getResult();
        result = unitValue(rewriter, location);
      } else {
        auto packedResult = mlir::LLVM::LLVMStructType::getLiteral(
            rewriter.getContext(), {rewriter.getI32Type(), convertedResult});
        auto call = mlir::LLVM::CallOp::create(
            rewriter, location, mlir::TypeRange{packedResult},
            operation.getCalleeAttr(), arguments);
        taskStatus = mlir::LLVM::ExtractValueOp::create(
            rewriter, location, rewriter.getI32Type(), call.getResult(),
            llvm::ArrayRef<int64_t>{0});
        auto userResult = mlir::LLVM::ExtractValueOp::create(
            rewriter, location, convertedResult, call.getResult(),
            llvm::ArrayRef<int64_t>{1});
        result = packKnownValue(rewriter, location, userResult, convertedResult);
      }
      auto releaseType = mlir::LLVM::LLVMFunctionType::get(
          mlir::LLVM::LLVMVoidType::get(rewriter.getContext()), {pointer});
      auto release = runtime.getOrInsert(trampoline, "dola_rt_codegen_release",
                                         releaseType);
      for (mlir::Value raw : rawArguments)
        mlir::LLVM::CallOp::create(
            rewriter, location, release,
            mlir::ValueRange{addressOfValue(rewriter, location, raw)});
      mlir::LLVM::StoreOp::create(rewriter, location, result,
                                  block->getArgument(2));
      mlir::LLVM::ReturnOp::create(rewriter, location,
                                   taskStatus);
    }

    auto trampolineAddress = mlir::LLVM::AddressOfOp::create(
        rewriter, location, pointer, trampoline.getSymNameAttr());
    auto output =
        mlir::LLVM::AllocaOp::create(rewriter, location, pointer, rawValue,
                                     constant(rewriter, location, 1, 64), 8);
    auto spawnType = mlir::LLVM::LLVMFunctionType::get(
        rewriter.getI32Type(),
        {pointer, pointer, pointer, rewriter.getI64Type(), pointer});
    auto spawn =
        runtime.getOrInsert(operation, "dola_rt_codegen_task_spawn", spawnType);
    mlir::LLVM::CallOp::create(
        rewriter, location, spawn,
        mlir::ValueRange{adaptor.getContext(), trampolineAddress,
                         addressOfValue(rewriter, location, environment),
                         constant(rewriter, location, 0, 64), output});
    auto releaseType = mlir::LLVM::LLVMFunctionType::get(
        mlir::LLVM::LLVMVoidType::get(rewriter.getContext()), {rawValue});
    auto release =
        runtime.getOrInsert(operation, "dola_rt_value_release", releaseType);
    mlir::LLVM::CallOp::create(rewriter, location, release,
                               mlir::ValueRange{environment});
    rewriter.replaceOpWithNewOp<mlir::LLVM::LoadOp>(operation, rawValue,
                                                    output);
    return mlir::success();
  }

private:
  static std::atomic_uint64_t nextTrampoline_;
};

std::atomic_uint64_t TaskSpawnLowering::nextTrampoline_{0};

struct RuntimeLowering : mlir::ConvertOpToLLVMPattern<ir::RuntimeOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;
  mlir::LogicalResult
  matchAndRewrite(ir::RuntimeOp operation, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    const auto runtimeOperation =
        ir::parseRuntimeOperation(operation.getCallee());
    if (!runtimeOperation)
      return rewriter.notifyMatchFailure(operation,
                                         "unknown runtime operation");
    auto pointer = mlir::LLVM::LLVMPointerType::get(rewriter.getContext());
    const int64_t count = static_cast<int64_t>(adaptor.getInputs().size());
    auto countValue =
        constant(rewriter, operation.getLoc(), count == 0 ? 1 : count, 64);
    auto inputs = mlir::LLVM::AllocaOp::create(
        rewriter, operation.getLoc(), pointer, valueType(rewriter.getContext()),
        countValue, 8);
    for (int64_t index = 0; index < count; ++index) {
      auto slot = mlir::LLVM::GEPOp::create(
          rewriter, operation.getLoc(), pointer,
          valueType(rewriter.getContext()), inputs,
          llvm::ArrayRef<mlir::LLVM::GEPArg>{static_cast<int32_t>(index)});
      mlir::LLVM::StoreOp::create(
          rewriter, operation.getLoc(),
          packValue(rewriter, operation.getLoc(),
                    adaptor.getInputs().drop_front(index).front()),
          slot);
    }
    auto one = constant(rewriter, operation.getLoc(), 1, 64);
    auto out =
        mlir::LLVM::AllocaOp::create(rewriter, operation.getLoc(), pointer,
                                     valueType(rewriter.getContext()), one, 8);
    auto metadata = operation.getMetadata();
    int64_t metadata0 = metadata.empty() ? 0 : metadata.front();
    int64_t metadata1 = metadata.size() < 2 ? 0 : metadata.drop_front().front();
    auto functionType = mlir::LLVM::LLVMFunctionType::get(
        rewriter.getI32Type(),
        {rewriter.getI32Type(), rewriter.getI64Type(), rewriter.getI64Type(),
         rewriter.getI64Type(), pointer, rewriter.getI64Type(), pointer});
    RuntimeCallBuilder runtime(rewriter);
    auto function =
        runtime.getOrInsert(operation, "dola_rt_codegen", functionType);
    mlir::LLVM::CallOp::create(
        rewriter, operation.getLoc(), function,
        mlir::ValueRange{
            constant(rewriter, operation.getLoc(),
                     static_cast<std::uint32_t>(*runtimeOperation), 32),
            constant(rewriter, operation.getLoc(),
                     static_cast<int64_t>(operation.getTypeId()), 64),
            constant(rewriter, operation.getLoc(), metadata0, 64),
            constant(rewriter, operation.getLoc(), metadata1, 64), inputs,
            constant(rewriter, operation.getLoc(), count, 64), out});
    if (operation.getNumResults() == 0) {
      rewriter.eraseOp(operation);
      return mlir::success();
    }
    auto raw = mlir::LLVM::LoadOp::create(
        rewriter, operation.getLoc(), valueType(rewriter.getContext()), out);
    auto converted =
        getTypeConverter()->convertType(operation.getResult(0).getType());
    rewriter.replaceOp(
        operation, unpackValue(rewriter, operation.getLoc(), raw, converted));
    return mlir::success();
  }
};

struct SlotAllocLowering : mlir::ConvertOpToLLVMPattern<ir::SlotAllocOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;
  mlir::LogicalResult
  matchAndRewrite(ir::SlotAllocOp operation, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    auto pointer = mlir::LLVM::LLVMPointerType::get(rewriter.getContext());
    auto one = constant(rewriter, operation.getLoc(), 1, 64);
    auto slot =
        mlir::LLVM::AllocaOp::create(rewriter, operation.getLoc(), pointer,
                                     adaptor.getInitial().getType(), one, 8);
    mlir::LLVM::StoreOp::create(rewriter, operation.getLoc(),
                                adaptor.getInitial(), slot);
    rewriter.replaceOp(operation, slot);
    return mlir::success();
  }
};

struct SlotLoadLowering : mlir::ConvertOpToLLVMPattern<ir::SlotLoadOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;
  mlir::LogicalResult
  matchAndRewrite(ir::SlotLoadOp operation, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    auto converted =
        getTypeConverter()->convertType(operation.getValue().getType());
    rewriter.replaceOpWithNewOp<mlir::LLVM::LoadOp>(operation, converted,
                                                    adaptor.getSlot());
    return mlir::success();
  }
};

struct SlotStoreLowering : mlir::ConvertOpToLLVMPattern<ir::SlotStoreOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;
  mlir::LogicalResult
  matchAndRewrite(ir::SlotStoreOp operation, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter& rewriter) const override {
    mlir::LLVM::StoreOp::create(rewriter, operation.getLoc(),
                                adaptor.getValue(), adaptor.getSlot());
    rewriter.eraseOp(operation);
    return mlir::success();
  }
};

class LowerToLlvmPass
    : public mlir::PassWrapper<LowerToLlvmPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerToLlvmPass)

  [[nodiscard]] llvm::StringRef getArgument() const final {
    return "dola-lower-to-llvm";
  }

  [[nodiscard]] llvm::StringRef getDescription() const final {
    return "lower Dola and standard dialects to the LLVM dialect";
  }

  void runOnOperation() override {
    auto* context = &getContext();
    mlir::RewritePatternSet scfPatterns(context);
    mlir::populateSCFToControlFlowConversionPatterns(scfPatterns);
    mlir::ConversionTarget scfTarget(*context);
    scfTarget.addIllegalDialect<mlir::scf::SCFDialect>();
    scfTarget.markUnknownOpDynamicallyLegal(
        [](mlir::Operation*) { return true; });
    if (mlir::failed(mlir::applyPartialConversion(
            getOperation(), scfTarget, std::move(scfPatterns)))) {
      signalPassFailure();
      return;
    }

    mlir::LLVMTypeConverter converter(context);
    converter.addConversion([](ir::SlotType type) -> mlir::Type {
      return mlir::LLVM::LLVMPointerType::get(type.getContext());
    });
    converter.addConversion([](ir::ContextType type) -> mlir::Type {
      return mlir::LLVM::LLVMPointerType::get(type.getContext());
    });
    converter.addConversion([](mlir::Type type) -> std::optional<mlir::Type> {
      if (mlir::isa<ir::StringType, ir::RecordType, ir::EnumType, ir::TupleType,
                    ir::OptionType, ir::ResultType, ir::ListType, ir::MapType,
                    ir::SenderType, ir::ReceiverType, ir::TaskType,
                    ir::ListenerType, ir::ConnectionType>(type))
        return valueType(type.getContext());
      return std::nullopt;
    });
    mlir::RewritePatternSet patterns(context);
    mlir::arith::populateArithToLLVMConversionPatterns(converter, patterns);
    mlir::cf::populateControlFlowToLLVMConversionPatterns(converter, patterns);
    mlir::populateFuncToLLVMConversionPatterns(converter, patterns);
    mlir::populateFinalizeMemRefToLLVMConversionPatterns(converter, patterns);
    patterns
        .add<StringLiteralLowering, PanicLowering, PoisonLowering,
             RetainLowering,
             ReleaseLowering, EqualLowering, RuntimeLowering, SlotAllocLowering,
             SlotLoadLowering, SlotStoreLowering, TaskSpawnLowering>(converter);
    patterns.add<OutputLowering<ir::PrintOp>>(converter, "dola_rt_print_value");
    patterns.add<OutputLowering<ir::PrintlnOp>>(converter,
                                                "dola_rt_println_value");

    mlir::LLVMConversionTarget target(*context);
    target.addLegalOp<mlir::ModuleOp>();
    if (mlir::failed(mlir::applyFullConversion(getOperation(), target,
                                               std::move(patterns))))
      signalPassFailure();
  }
};

void synthesizeEntry(llvm::Module& module, llvm::StringRef entrySymbol,
                     EntryResultKind entryResult) {
  auto& context = module.getContext();
  llvm::IRBuilder<> builder(context);
  auto pointerType = builder.getPtrTy();
  auto statusType = builder.getInt32Ty();
  auto createType = llvm::FunctionType::get(statusType, {pointerType}, false);
  auto destroyType =
      llvm::FunctionType::get(builder.getVoidTy(), {pointerType}, false);
  auto create = module.getOrInsertFunction("dola_rt_create", createType);
  auto destroy = module.getOrInsertFunction("dola_rt_destroy", destroyType);
  auto contextCreate = module.getOrInsertFunction(
      "dola_rt_context_create",
      llvm::FunctionType::get(statusType, {pointerType, pointerType}, false));
  auto contextDestroy = module.getOrInsertFunction(
      "dola_rt_context_destroy",
      llvm::FunctionType::get(builder.getVoidTy(), {pointerType}, false));
  auto reportPanic = module.getOrInsertFunction(
      "dola_rt_context_report_panic",
      llvm::FunctionType::get(statusType, {pointerType}, false));
  auto mainType = llvm::FunctionType::get(statusType, {}, false);
  auto* main = llvm::Function::Create(
      mainType, llvm::GlobalValue::ExternalLinkage, "main", module);
  auto* entry = llvm::BasicBlock::Create(context, "entry", main);
  auto* runtimeReady = llvm::BasicBlock::Create(context, "runtime_ready", main);
  auto* run = llvm::BasicBlock::Create(context, "run", main);
  auto* contextFailed = llvm::BasicBlock::Create(context, "context_failed", main);
  auto* panicked = llvm::BasicBlock::Create(context, "panicked", main);
  auto* completed = llvm::BasicBlock::Create(context, "completed", main);
  auto* initializationFailed =
      llvm::BasicBlock::Create(context, "init_failed", main);
  builder.SetInsertPoint(entry);
  auto runtime = builder.CreateAlloca(pointerType);
  builder.CreateStore(llvm::ConstantPointerNull::get(pointerType), runtime);
  auto status = builder.CreateCall(create, {runtime});
  builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)),
                       runtimeReady, initializationFailed);
  builder.SetInsertPoint(initializationFailed);
  builder.CreateRet(status);
  builder.SetInsertPoint(runtimeReady);
  auto executionContext = builder.CreateAlloca(pointerType);
  builder.CreateStore(llvm::ConstantPointerNull::get(pointerType),
                      executionContext);
  llvm::Value* contextStatus = builder.CreateCall(
      contextCreate,
      {builder.CreateLoad(pointerType, runtime), executionContext});
  builder.CreateCondBr(
      builder.CreateICmpEQ(contextStatus, builder.getInt32(0)), run,
      contextFailed);
  builder.SetInsertPoint(contextFailed);
  builder.CreateCall(destroy, {builder.CreateLoad(pointerType, runtime)});
  builder.CreateRet(contextStatus);

  builder.SetInsertPoint(run);
  llvm::Type* userResultType = nullptr;
  if (entryResult == EntryResultKind::Int)
    userResultType = builder.getInt64Ty();
  else if (entryResult == EntryResultKind::Result)
    userResultType =
        llvm::StructType::get(statusType, statusType, builder.getInt64Ty());
  llvm::Type* targetResultType = statusType;
  if (userResultType != nullptr)
    targetResultType = llvm::StructType::get(statusType, userResultType);
  auto entryType = llvm::FunctionType::get(
      targetResultType, {pointerType}, false);
  auto target = module.getOrInsertFunction(entrySymbol, entryType);
  auto result = builder.CreateCall(
      target, {builder.CreateLoad(pointerType, executionContext)});
  llvm::Value* callStatus = result;
  llvm::Value* userResult = nullptr;
  if (userResultType != nullptr) {
    callStatus = builder.CreateExtractValue(result, 0);
    userResult = builder.CreateExtractValue(result, 1);
  }
  builder.CreateCondBr(
      builder.CreateICmpEQ(callStatus, builder.getInt32(0)), completed,
      panicked);

  builder.SetInsertPoint(panicked);
  builder.CreateCall(reportPanic,
                     {builder.CreateLoad(pointerType, executionContext)});
  builder.CreateCall(contextDestroy,
                     {builder.CreateLoad(pointerType, executionContext)});
  builder.CreateCall(destroy, {builder.CreateLoad(pointerType, runtime)});
  builder.CreateRet(builder.getInt32(1));

  builder.SetInsertPoint(completed);
  llvm::Value* exitStatus = builder.getInt32(0);
  if (entryResult == EntryResultKind::Result) {
    auto resultStorage = builder.CreateAlloca(userResultType);
    builder.CreateStore(userResult, resultStorage);
    auto handlerType =
        llvm::FunctionType::get(statusType, {pointerType}, false);
    auto handler =
        module.getOrInsertFunction("dola_rt_result_main", handlerType);
    exitStatus = builder.CreateCall(handler, {resultStorage});
  } else if (entryResult == EntryResultKind::Int) {
    exitStatus = builder.CreateTrunc(userResult, builder.getInt32Ty());
  }
  builder.CreateCall(contextDestroy,
                     {builder.CreateLoad(pointerType, executionContext)});
  builder.CreateCall(destroy, {builder.CreateLoad(pointerType, runtime)});
  builder.CreateRet(exitStatus);
}

} // namespace

std::unique_ptr<mlir::Pass> createLowerToLlvmPass() {
  return std::make_unique<LowerToLlvmPass>();
}

void registerDolaLoweringPass() {
  static const mlir::PassRegistration<LowerToLlvmPass> registration;
  (void)registration;
}

bool lowerToLlvmDialect(mlir::ModuleOp module, std::string& error) {
  mlir::PassManager manager(module.getContext());
  manager.addPass(createLowerToLlvmPass());
  if (mlir::failed(manager.run(module))) {
    error = "failed to lower module to the LLVM dialect";
    return false;
  }
  if (mlir::failed(mlir::verify(module))) {
    error = "lowered LLVM dialect module failed verification";
    return false;
  }
  return true;
}

std::unique_ptr<llvm::Module> translateToLlvmIr(mlir::ModuleOp module,
                                                mlir::MLIRContext& context,
                                                const std::string& entrySymbol,
                                                EntryResultKind entryResult,
                                                std::string& error) {
  mlir::registerBuiltinDialectTranslation(context);
  mlir::registerLLVMDialectTranslation(context);
  static llvm::LLVMContext llvmContext;
  auto translated = mlir::translateModuleToLLVMIR(module, llvmContext);
  if (!translated) {
    error = "failed to translate LLVM dialect to LLVM IR";
    return nullptr;
  }
  synthesizeEntry(*translated, entrySymbol, entryResult);
  return translated;
}

bool emitNativeObject(llvm::Module& module, const std::filesystem::path& path,
                      std::string& error) {
  static const bool initialized = [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    return true;
  }();
  (void)initialized;
  const llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
  std::string lookupError;
  const llvm::Target* target =
      llvm::TargetRegistry::lookupTarget(triple, lookupError);
  if (!target) {
    error = lookupError;
    return false;
  }
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> targetMachine(
      target->createTargetMachine(triple, "generic", "", options,
                                  llvm::Reloc::PIC_));
  module.setTargetTriple(triple);
  module.setDataLayout(targetMachine->createDataLayout());
  module.setPICLevel(llvm::PICLevel::BigPIC);
  std::error_code fileError;
  llvm::raw_fd_ostream output(path.string(), fileError, llvm::sys::fs::OF_None);
  if (fileError) {
    error = fileError.message();
    return false;
  }
  llvm::legacy::PassManager passes;
  if (targetMachine->addPassesToEmitFile(passes, output, nullptr,
                                         llvm::CodeGenFileType::ObjectFile)) {
    error = "target does not support object emission";
    return false;
  }
  passes.run(module);
  output.flush();
  return true;
}

} // namespace dola
