#include "dola/compiler_driver.h"

#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace dola {
namespace {

TEST(CompilerDriverTest, RunsAstPhaseWithExplicitOptions) {
  const char* temporary = std::getenv("TEST_TMPDIR");
  const std::filesystem::path path =
      std::filesystem::path(temporary ? temporary : "/tmp") / "driver.dola";
  std::ofstream(path) << "module driver; fn main() {}\n";
  DriverOptions options{.mode = DriverMode::Ast, .sourcePaths = {path}};
  std::string output;
  llvm::raw_string_ostream outputStream(output);
  std::ostringstream error;
  EXPECT_EQ(runCompiler(options, outputStream, error), 0);
  outputStream.flush();
  EXPECT_NE(output.find("(module driver"), std::string::npos);
  EXPECT_TRUE(error.str().empty());
}

TEST(CompilerDriverTest, ReportsMissingSource) {
  DriverOptions options{.mode = DriverMode::Check,
                        .sourcePaths = {"does-not-exist.dola"}};
  std::string output;
  llvm::raw_string_ostream outputStream(output);
  std::ostringstream error;
  EXPECT_EQ(runCompiler(options, outputStream, error), 1);
  EXPECT_NE(error.str().find("cannot read source file"), std::string::npos);
}

TEST(DriverOptionsTest, ParsesModesWithLlvmCommandLine) {
  const std::array arguments = {"dola",      "--emit-object", "-o",
                                "program.o", "main.dola",     "library.dola"};
  std::string errors;
  llvm::raw_string_ostream errorStream(errors);
  DriverOptionResult result = parseDriverOptions(
      static_cast<int>(arguments.size()), arguments.data(), errorStream);

  ASSERT_TRUE(result.options);
  if (!result.options)
    return;
  EXPECT_EQ(result.options->mode, DriverMode::Object);
  EXPECT_EQ(result.options->outputPath, std::filesystem::path("program.o"));
  EXPECT_EQ(result.options->sourcePaths,
            (std::vector<std::filesystem::path>{"main.dola", "library.dola"}));
  EXPECT_TRUE(errors.empty());
}

} // namespace
} // namespace dola
