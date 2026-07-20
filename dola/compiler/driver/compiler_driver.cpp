#include "dola/compiler_driver.h"

#include "dola/ast.h"
#include "dola/diagnostic.h"
#include "dola/dola_dialect.h"
#include "dola/lowering.h"
#include "dola/mlir_gen.h"
#include "dola/parser.h"
#include "dola/sema.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <optional>
#include <sstream>

namespace dola {
namespace {

std::optional<std::string> readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

} // namespace

int runCompiler(const DriverOptions& options, llvm::raw_ostream& output,
                std::ostream& errorOutput) {
  DiagnosticEngine diagnostics;
  std::vector<Module> modules;
  for (const auto& path : options.sourcePaths) {
    auto source = readFile(path);
    if (!source) {
      diagnostics.error(SourceLocation{.path = path},
                        "cannot read source file");
      continue;
    }
    auto module = parseSource(path, *source, diagnostics);
    if (module)
      modules.push_back(std::move(*module));
  }
  if (diagnostics.hasErrors()) {
    diagnostics.print(errorOutput);
    return 1;
  }
  if (options.mode == DriverMode::Ast) {
    for (const auto& module : modules)
      output << dumpAst(module);
    return 0;
  }
  const bool requiresEntry =
      options.mode == DriverMode::Llvm || options.mode == DriverMode::Object;
  auto semantics = analyze(modules, diagnostics, requiresEntry);
  if (!semantics) {
    diagnostics.print(errorOutput);
    return 1;
  }
  if (options.mode == DriverMode::Check)
    return 0;

  mlir::MLIRContext context;
  context.loadDialect<ir::DolaDialect, mlir::arith::ArithDialect,
                      mlir::cf::ControlFlowDialect, mlir::func::FuncDialect,
                      mlir::LLVM::LLVMDialect, mlir::memref::MemRefDialect,
                      mlir::scf::SCFDialect>();
  auto generated = generateMlir(context, modules, *semantics);
  if (!generated)
    return 1;
  if (mlir::failed(mlir::verify(*generated)))
    return 1;
  if (options.mode == DriverMode::Mlir) {
    generated->print(output);
    output << '\n';
    return 0;
  }

  std::string error;
  if (!lowerToLlvmDialect(*generated, error)) {
    errorOutput << "dola: " << error << '\n';
    return 1;
  }
  if (options.mode == DriverMode::LoweredMlir) {
    generated->print(output);
    output << '\n';
    return 0;
  }

  if (!semantics->entryPoint) {
    errorOutput << "dola: executable entry point was not resolved\n";
    return 1;
  }
  const FunctionRef entry = semantics->entryPoint.value();
  const auto& entryModule = at(modules, entry.module);
  const auto& entryFunction = at(entryModule.functions, entry.function);
  const TypeId entryResult = semantics->functions.at(entryFunction.id).result;
  EntryResultKind entryKind = EntryResultKind::Unit;
  const TypeKind entryType =
      semantics->typeTable.types.at(entryResult.value()).kind;
  if (entryType == TypeKind::Int)
    entryKind = EntryResultKind::Int;
  else if (entryType == TypeKind::Result)
    entryKind = EntryResultKind::Result;
  auto llvmModule = translateToLlvmIr(
      *generated, context, mangleFunction(entryModule, entryFunction),
      entryKind, error);
  if (!llvmModule) {
    errorOutput << "dola: " << error << '\n';
    return 1;
  }
  if (options.mode == DriverMode::Llvm) {
    llvmModule->print(output, nullptr);
    return 0;
  }
  if (!emitNativeObject(*llvmModule, options.outputPath, error)) {
    errorOutput << "dola: " << error << '\n';
    return 1;
  }
  return 0;
}

} // namespace dola
