#ifndef DOLA_DRIVER_DRIVER_OPTIONS_H
#define DOLA_DRIVER_DRIVER_OPTIONS_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace dola {

enum class DriverMode : std::uint8_t {
  Check,
  Ast,
  Mlir,
  LoweredMlir,
  Llvm,
  Object,
};

struct DriverOptions {
  DriverMode mode = DriverMode::Check;
  std::vector<std::filesystem::path> sourcePaths;
  std::filesystem::path outputPath;
};

struct DriverOptionResult {
  std::optional<DriverOptions> options;
};

DriverOptionResult parseDriverOptions(int argc, const char* const* argv,
                                      llvm::raw_ostream& errors);

} // namespace dola
#endif
