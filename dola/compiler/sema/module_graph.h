#ifndef DOLA_SEMA_MODULE_GRAPH_H
#define DOLA_SEMA_MODULE_GRAPH_H

#include "dola/diagnostic.h"
#include "dola/sema.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dola::sema::detail {

struct TypeDeclRef {
  ModuleId module;
  std::variant<RecordIndex, EnumIndex> declaration;
};

class ModuleGraph {
public:
  enum class BuiltinModule : std::uint8_t { Io, Time, Transport };
  using ResolvedImport = std::variant<ModuleId, BuiltinModule>;
  using ImportTarget = std::optional<ResolvedImport>;

  ModuleGraph(const std::vector<Module>& modules,
              DiagnosticEngine& diagnostics);

  void build();
  const ImportTarget* findImport(ModuleId module,
                                 const std::string& alias) const;
  bool hasBuiltinImport(ModuleId module, const std::string& alias,
                        BuiltinModule builtin) const;
  std::optional<FunctionRef> findFunction(ModuleId module,
                                          const std::string& name) const;
  std::optional<TypeDeclRef> findType(ModuleId module,
                                      const std::string& name) const;
  std::vector<FunctionRef> functionsNamed(const std::string& name) const;

private:
  void collectModules();
  void collectDeclarations();
  void collectImportsAndCheckCycles();

  const std::vector<Module>& modules_;
  DiagnosticEngine& diagnostics_;
  std::unordered_map<std::string, ModuleId> moduleByName_;
  std::vector<std::unordered_map<std::string, FunctionRef>> functions_;
  std::vector<std::unordered_map<std::string, TypeDeclRef>> types_;
  std::vector<std::unordered_map<std::string, ImportTarget>> imports_;
};

} // namespace dola::sema::detail

#endif
