#ifndef DOLA_SUPPORT_DIAGNOSTIC_H
#define DOLA_SUPPORT_DIAGNOSTIC_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

namespace dola {

struct SourceLocation {
  std::filesystem::path path;
  uint32_t line = 1;
  uint32_t column = 1;
  uint32_t length = 1;
};

struct Diagnostic {
  SourceLocation location;
  std::string message;
};

class DiagnosticEngine {
public:
  void registerSource(std::filesystem::path path, std::string source);
  void error(SourceLocation location, std::string message);
  [[nodiscard]] bool hasErrors() const { return !diagnostics_.empty(); }
  [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
    return diagnostics_;
  }
  void print(std::ostream& output) const;

private:
  std::vector<Diagnostic> diagnostics_;
  std::unordered_map<std::filesystem::path, std::string> sources_;
  std::unordered_map<std::filesystem::path, size_t> sourceOrder_;
  size_t nextSourceOrder_ = 0;
};

} // namespace dola
#endif
