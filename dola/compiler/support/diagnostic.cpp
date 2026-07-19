#include "dola/diagnostic.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <ranges>
#include <sstream>
#include <tuple>
#include <utility>

namespace dola {

void DiagnosticEngine::registerSource(std::filesystem::path path,
                                      std::string source) {
  if (!sourceOrder_.contains(path))
    sourceOrder_[path] = nextSourceOrder_++;
  sources_.insert_or_assign(std::move(path), std::move(source));
}

void DiagnosticEngine::error(SourceLocation location, std::string message) {
  diagnostics_.push_back(
      {.location = std::move(location), .message = std::move(message)});
}

void DiagnosticEngine::print(std::ostream& output) const {
  std::vector<const Diagnostic*> ordered;
  ordered.reserve(diagnostics_.size());
  for (const auto& diagnostic : diagnostics_)
    ordered.push_back(&diagnostic);
  // The comparator uses source ranks and locations, never pointer values.
  std::ranges::
      stable_sort( // NOLINT(bugprone-nondeterministic-pointer-iteration-order)
          ordered, [&](const auto* a, const auto* b) {
            auto rank = [&](const std::filesystem::path& path) {
              auto found = sourceOrder_.find(path);
              return found == sourceOrder_.end()
                         ? std::numeric_limits<size_t>::max()
                         : found->second;
            };
            return std::tuple{rank(a->location.path), a->location.line,
                              a->location.column, a->location.path} <
                   std::tuple{rank(b->location.path), b->location.line,
                              b->location.column, b->location.path};
          });
  for (const Diagnostic* diagnostic : ordered) {
    const auto& loc = diagnostic->location;
    output << loc.path.string() << ':' << loc.line << ':' << loc.column
           << ": error: " << diagnostic->message << '\n';
    auto source = sources_.find(loc.path);
    if (source == sources_.end())
      continue;
    std::istringstream lines(source->second);
    std::string line;
    for (uint32_t current = 1; current <= loc.line && std::getline(lines, line);
         ++current) {
      if (current != loc.line)
        continue;
      output << "    " << line << '\n' << "    ";
      for (uint32_t column = 1; column < loc.column; ++column)
        output << ' ';
      output << std::string(std::max(1u, loc.length), '^') << '\n';
    }
  }
}

} // namespace dola
