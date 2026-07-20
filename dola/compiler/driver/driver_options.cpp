#include "dola/driver_options.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

namespace dola {

DriverOptionResult parseDriverOptions(int argc, const char* const* argv,
                                      llvm::raw_ostream& errors) {
  llvm::cl::OptionCategory category("Dola compiler options");
  llvm::cl::opt<DriverMode> mode(
      llvm::cl::desc("Compiler action"), llvm::cl::cat(category),
      llvm::cl::values(
          clEnumValN(DriverMode::Check, "check", "Check the source program"),
          clEnumValN(DriverMode::Ast, "dump-ast", "Print the parsed AST"),
          clEnumValN(DriverMode::Mlir, "emit-mlir", "Print high-level MLIR"),
          clEnumValN(DriverMode::LoweredMlir, "emit-lowered-mlir",
                     "Print LLVM-dialect MLIR"),
          clEnumValN(DriverMode::Llvm, "emit-llvm", "Print LLVM IR"),
          clEnumValN(DriverMode::Object, "emit-object",
                     "Emit a native object file")),
      llvm::cl::init(DriverMode::Check));
  llvm::cl::opt<std::string> rawOutputPath("o", llvm::cl::desc("Output file"),
                                           llvm::cl::value_desc("path"),
                                           llvm::cl::cat(category));
  llvm::cl::list<std::string> rawSourcePaths(
      llvm::cl::Positional, llvm::cl::desc("<source files>"),
      llvm::cl::OneOrMore, llvm::cl::cat(category));

  llvm::cl::HideUnrelatedOptions(category);
  if (!llvm::cl::ParseCommandLineOptions(argc, argv, "Dola compiler\n",
                                         &errors))
    return {};
  if (mode.getNumOccurrences() > 1) {
    errors << argv[0] << ": exactly one compiler action may be specified\n";
    return {};
  }
  if (mode == DriverMode::Object && rawOutputPath.empty()) {
    errors << argv[0] << ": --emit-object requires -o <path>\n";
    return {};
  }
  DriverOptions options;
  options.mode = mode;
  options.outputPath = std::filesystem::path(rawOutputPath.getValue());
  options.sourcePaths.reserve(rawSourcePaths.size());
  for (const std::string& rawPath : rawSourcePaths)
    options.sourcePaths.emplace_back(rawPath);
  return {.options = std::move(options)};
}

} // namespace dola
