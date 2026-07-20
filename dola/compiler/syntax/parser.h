#ifndef DOLA_SYNTAX_PARSER_H
#define DOLA_SYNTAX_PARSER_H

#include "dola/ast.h"
#include "dola/diagnostic.h"

#include <filesystem>
#include <optional>
#include <string>

namespace dola {
std::optional<Module> parseSource(const std::filesystem::path& path,
                                  const std::string& source,
                                  DiagnosticEngine& diagnostics);
} // namespace dola
#endif
