#include "dola/dola_dialect.h"
#include "dola/lowering.h"
#include "dola/mlir_gen.h"
#include "dola/parser.h"
#include "dola/runtime_operations.h"
#include "dola/sema.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/IR/Module.h"
#include "gtest/gtest.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

namespace dola {
namespace {

TEST(MlirApiTest, VerifiesLowersTranslatesAndCreatesObject) {
  DiagnosticEngine diagnostics;
  auto parsed = parseSource(
      "smoke.dola",
      "module smoke; use std.io; fn identity(value: String) -> String { value "
      "} "
      "fn main() -> Int { var answer = 40; answer = answer + 2; "
      "io.println(identity(\"hello\")); if answer == 42 { 0 } else { 1 } }",
      diagnostics);
  ASSERT_TRUE(parsed);
  if (!parsed)
    return;
  std::vector<Module> sources;
  sources.push_back(std::move(*parsed));
  auto semantics = analyze(sources, diagnostics, true);
  ASSERT_TRUE(semantics);
  if (!semantics)
    return;

  mlir::MLIRContext context;
  context.loadDialect<ir::DolaDialect, mlir::arith::ArithDialect,
                      mlir::cf::ControlFlowDialect, mlir::func::FuncDialect,
                      mlir::LLVM::LLVMDialect, mlir::memref::MemRefDialect,
                      mlir::scf::SCFDialect>();
  auto module = generateMlir(context, sources, *semantics);
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*module)));
  std::string error;
  ASSERT_TRUE(lowerToLlvmDialect(*module, error)) << error;
  auto llvmModule = translateToLlvmIr(*module, context, "__dola_smoke_main",
                                      EntryResultKind::Int, error);
  ASSERT_TRUE(llvmModule) << error;
  const char* temporary = std::getenv("TEST_TMPDIR");
  std::filesystem::path objectPath =
      std::filesystem::path(temporary ? temporary : "/tmp") / "dola-smoke.o";
  EXPECT_TRUE(emitNativeObject(*llvmModule, objectPath, error)) << error;
  EXPECT_EQ(llvmModule->getPICLevel(), llvm::PICLevel::BigPIC);
  EXPECT_FALSE(emitNativeObject(
      *llvmModule, std::filesystem::path(temporary ? temporary : "/tmp"),
      error));
  EXPECT_FALSE(error.empty());
}

TEST(MlirApiTest, RejectsIllegalOperationsDuringLowering) {
  mlir::MLIRContext context;
  context.disableMultithreading();
  context.loadDialect<ir::DolaDialect, mlir::arith::ArithDialect,
                      mlir::cf::ControlFlowDialect, mlir::func::FuncDialect,
                      mlir::LLVM::LLVMDialect, mlir::memref::MemRefDialect,
                      mlir::scf::SCFDialect>();
  context.allowUnregisteredDialects();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(
      "module { \"test.illegal\"() : () -> () }", &context);
  ASSERT_TRUE(module);
  std::string error;
  EXPECT_FALSE(lowerToLlvmDialect(*module, error));
  EXPECT_EQ(error, "failed to lower module to the LLVM dialect");
}

TEST(MlirApiTest, LowersUnitIfContainingMatchWithoutInvalidScfRegions) {
  DiagnosticEngine diagnostics;
  auto parsed = parseSource(
      "nested-control.dola",
      "module nested.control; enum Choice { A, B, } "
      "fn consume(choice: Choice) -> Unit { if true { match choice { "
      "Choice.A => (), Choice.B => (), }; } else { () }; } "
      "fn main() -> Unit { consume(Choice.A); }",
      diagnostics);
  ASSERT_TRUE(parsed);
  if (!parsed)
    return;
  std::vector<Module> sources;
  sources.push_back(std::move(*parsed));
  auto semantics = analyze(sources, diagnostics, true);
  ASSERT_TRUE(semantics);
  if (!semantics)
    return;

  mlir::MLIRContext context;
  context.loadDialect<ir::DolaDialect, mlir::arith::ArithDialect,
                      mlir::cf::ControlFlowDialect, mlir::func::FuncDialect,
                      mlir::LLVM::LLVMDialect, mlir::memref::MemRefDialect,
                      mlir::scf::SCFDialect>();
  auto module = generateMlir(context, sources, *semantics);
  ASSERT_TRUE(module);
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*module)));
  std::string error;
  EXPECT_TRUE(lowerToLlvmDialect(*module, error)) << error;
}

TEST(MlirApiTest, RejectsNonLlvmOperationsDuringTranslation) {
  mlir::MLIRContext context;
  context.disableMultithreading();
  context.loadDialect<ir::DolaDialect, mlir::arith::ArithDialect,
                      mlir::cf::ControlFlowDialect, mlir::func::FuncDialect,
                      mlir::LLVM::LLVMDialect, mlir::memref::MemRefDialect,
                      mlir::scf::SCFDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(
      "module { %value = arith.constant 1 : i64 }", &context);
  ASSERT_TRUE(module);
  std::string error;
  EXPECT_FALSE(translateToLlvmIr(*module, context, "missing",
                                 EntryResultKind::Unit, error));
  EXPECT_EQ(error, "failed to translate LLVM dialect to LLVM IR");
}

TEST(MlirApiTest, RuntimeRegistryHasUniqueStableOperationCodes) {
  const auto descriptors = ir::runtimeOperationDescriptors();
  ASSERT_EQ(descriptors.size(), 48U);
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    EXPECT_EQ(static_cast<std::size_t>(descriptors[index].operation), index);
    EXPECT_EQ(ir::parseRuntimeOperation(descriptors[index].name),
              descriptors[index].operation);
  }
  EXPECT_EQ(static_cast<std::uint32_t>(ir::RuntimeOperation::Panic), 45U);
  EXPECT_EQ(static_cast<std::uint32_t>(ir::RuntimeOperation::StringConcat),
            46U);
  EXPECT_EQ(static_cast<std::uint32_t>(ir::RuntimeOperation::IntToString),
            47U);
}

TEST(MlirApiTest, RecursiveVerificationRejectsMalformedNestedRuntimeOp) {
  mlir::MLIRContext context;
  context.loadDialect<ir::DolaDialect, mlir::func::FuncDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(R"mlir(
    module {
      func.func @nested() {
        return
      }
    }
  )mlir", &context);
  ASSERT_TRUE(module);
  auto function = module->lookupSymbol<mlir::func::FuncOp>("nested");
  ASSERT_TRUE(function);
  mlir::OpBuilder builder(function.getBody());
  builder.setInsertionPoint(&function.front().back());
  builder.create<ir::RuntimeOp>(builder.getUnknownLoc(), mlir::TypeRange{},
                                "string.concat", 0,
                                llvm::ArrayRef<std::int32_t>{},
                                mlir::ValueRange{});
  EXPECT_TRUE(mlir::succeeded(module->verify()));
  EXPECT_TRUE(mlir::failed(mlir::verify(*module)));
}

} // namespace
} // namespace dola
