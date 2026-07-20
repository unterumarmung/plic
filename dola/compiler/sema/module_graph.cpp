#include "module_graph.h"

#include <functional>

namespace dola::sema::detail {

ModuleGraph::ModuleGraph(const std::vector<Module>& modules,
                         DiagnosticEngine& diagnostics)
    : modules_(modules), diagnostics_(diagnostics), functions_(modules.size()),
      types_(modules.size()), imports_(modules.size()) {}

void ModuleGraph::build() {
  collectModules();
  collectDeclarations();
  collectImportsAndCheckCycles();
}

void ModuleGraph::collectModules() {
  for (std::size_t index = 0; index < modules_.size(); ++index) {
    const ModuleId module{index};
    const std::string name = qualifiedName(at(modules_, module).name);
    if (!moduleByName_.emplace(name, module).second)
      diagnostics_.error(at(modules_, module).location,
                         "duplicate module `" + name + "`");
  }
}

void ModuleGraph::collectDeclarations() {
  for (std::size_t moduleIndex = 0; moduleIndex < modules_.size();
       ++moduleIndex) {
    const ModuleId module{moduleIndex};
    const auto& sourceModule = at(modules_, module);
    std::unordered_map<std::string, SourceLocation> names;
    for (std::size_t index = 0; index < sourceModule.functions.size();
         ++index) {
      const FunctionIndex function{index};
      const auto& declaration = at(sourceModule.functions, function);
      if (!names.emplace(declaration.name, declaration.location).second)
        diagnostics_.error(declaration.location,
                           "duplicate declaration `" + declaration.name + "`");
      functions_.at(module.value())
          .insert_or_assign(
              declaration.name,
              FunctionRef{.module = module, .function = function});
    }
    for (std::size_t index = 0; index < sourceModule.records.size(); ++index) {
      const RecordIndex record{index};
      const auto& declaration = at(sourceModule.records, record);
      if (!names.emplace(declaration.name, declaration.location).second)
        diagnostics_.error(declaration.location,
                           "duplicate declaration `" + declaration.name + "`");
      types_.at(module.value())
          .insert_or_assign(
              declaration.name,
              TypeDeclRef{.module = module, .declaration = record});
    }
    for (std::size_t index = 0; index < sourceModule.enums.size(); ++index) {
      const EnumIndex enumeration{index};
      const auto& declaration = at(sourceModule.enums, enumeration);
      if (!names.emplace(declaration.name, declaration.location).second)
        diagnostics_.error(declaration.location,
                           "duplicate declaration `" + declaration.name + "`");
      types_.at(module.value())
          .insert_or_assign(
              declaration.name,
              TypeDeclRef{.module = module, .declaration = enumeration});
    }
  }
}

void ModuleGraph::collectImportsAndCheckCycles() {
  std::vector<std::vector<ModuleId>> edges(modules_.size());
  for (std::size_t moduleIndex = 0; moduleIndex < modules_.size();
       ++moduleIndex) {
    const ModuleId module{moduleIndex};
    for (const auto& import : at(modules_, module).imports) {
      const std::string full = qualifiedName(import.moduleName);
      const std::string alias = import.moduleName.back();
      ImportTarget target;
      if (full == "std.io") {
        target = BuiltinModule::Io;
      } else if (full == "std.time") {
        target = BuiltinModule::Time;
      } else if (full == "std.transport") {
        target = BuiltinModule::Transport;
      } else if (auto found = moduleByName_.find(full);
                 found != moduleByName_.end()) {
        target = ResolvedImport{found->second};
        edges.at(module.value()).push_back(found->second);
      } else {
        diagnostics_.error(import.location, "unknown module `" + full + "`");
        target = std::nullopt;
      }
      if (!imports_.at(module.value()).emplace(alias, target).second)
        diagnostics_.error(import.location,
                           "duplicate import alias `" + alias + "`");
    }
  }
  std::vector<uint8_t> state(modules_.size());
  std::function<void(ModuleId)> visit = [&](ModuleId module) {
    state.at(module.value()) = 1;
    for (ModuleId target : edges.at(module.value())) {
      if (state.at(target.value()) == 1)
        diagnostics_.error({.path = at(modules_, module).path},
                           "module import cycle involving `" +
                               qualifiedName(at(modules_, target).name) + "`");
      else if (state.at(target.value()) == 0)
        visit(target);
    }
    state.at(module.value()) = 2;
  };
  for (std::size_t index = 0; index < modules_.size(); ++index) {
    const ModuleId module{index};
    if (state.at(index) == 0)
      visit(module);
  }
}

const ModuleGraph::ImportTarget*
ModuleGraph::findImport(ModuleId module, const std::string& alias) const {
  const auto& imports = imports_.at(module.value());
  auto found = imports.find(alias);
  return found == imports.end() ? nullptr : &found->second;
}

bool ModuleGraph::hasBuiltinImport(ModuleId module, const std::string& alias,
                                   BuiltinModule builtin) const {
  const auto* target = findImport(module, alias);
  return target && *target && std::holds_alternative<BuiltinModule>(**target) &&
         std::get<BuiltinModule>(**target) == builtin;
}

std::optional<FunctionRef>
ModuleGraph::findFunction(ModuleId module, const std::string& name) const {
  const auto& functions = functions_.at(module.value());
  auto found = functions.find(name);
  return found == functions.end() ? std::nullopt : std::optional(found->second);
}

std::optional<TypeDeclRef>
ModuleGraph::findType(ModuleId module, const std::string& name) const {
  const auto& types = types_.at(module.value());
  auto found = types.find(name);
  return found == types.end() ? std::nullopt : std::optional(found->second);
}

std::vector<FunctionRef>
ModuleGraph::functionsNamed(const std::string& name) const {
  std::vector<FunctionRef> result;
  for (const auto& functions : functions_)
    if (auto found = functions.find(name); found != functions.end())
      result.push_back(found->second);
  return result;
}

} // namespace dola::sema::detail
