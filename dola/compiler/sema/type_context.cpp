#include "type_context.h"

#include "dola/overloaded.h"

#include <algorithm>
#include <ranges>

namespace dola {

std::string TypeTable::name(TypeId id,
                            const std::vector<Module>& modules) const {
  const auto& type = at(types, id);
  auto parameterized = [&](std::string prefix) {
    prefix += '[';
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      if (index != 0)
        prefix += ", ";
      prefix += name(type.arguments.at(index), modules);
    }
    return prefix + ']';
  };
  auto declarationName = [&] {
    return std::visit(
        Overloaded{[&](RecordRef declaration) {
                     return at(at(modules, declaration.module).records,
                               declaration.record)
                         .name;
                   },
                   [&](EnumRef declaration) {
                     return at(at(modules, declaration.module).enums,
                               declaration.enumeration)
                         .name;
                   }},
        *type.declaration);
  };
  switch (type.kind) {
  case TypeKind::Unit:
    return "Unit";
  case TypeKind::Bool:
    return "Bool";
  case TypeKind::Int:
    return "Int";
  case TypeKind::Float:
    return "Float";
  case TypeKind::String:
    return "String";
  case TypeKind::Record:
  case TypeKind::Enum:
    return declarationName();
  case TypeKind::Tuple: {
    std::string result = "(";
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      if (index != 0)
        result += ", ";
      result += name(type.arguments.at(index), modules);
    }
    return result + ')';
  }
  case TypeKind::Option:
    return parameterized("Option");
  case TypeKind::Result:
    return parameterized("Result");
  case TypeKind::List:
    return parameterized("List");
  case TypeKind::Map:
    return parameterized("Map");
  case TypeKind::Sender:
    return parameterized("Sender");
  case TypeKind::Receiver:
    return parameterized("Receiver");
  case TypeKind::Task:
    return parameterized("Task");
  case TypeKind::Listener:
    return parameterized("Listener");
  case TypeKind::Connection:
    return parameterized("Connection");
  case TypeKind::Range:
    return "<range>";
  case TypeKind::IndexError:
    return "IndexError";
  case TypeKind::ChannelError:
    return "ChannelError";
  case TypeKind::TaskError:
    return "TaskError";
  case TypeKind::IoError:
    return "IoError";
  case TypeKind::TransportError:
    return "TransportError";
  case TypeKind::ParseIntError:
    return "ParseIntError";
  case TypeKind::Function:
    return "<function>";
  case TypeKind::Never:
    return "<never>";
  case TypeKind::Error:
    return "<error>";
  }
  return "<invalid>";
}

bool TypeTable::runtimeBacked(TypeId id) const {
  switch (at(types, id).kind) {
  case TypeKind::String:
  case TypeKind::Record:
  case TypeKind::Enum:
  case TypeKind::Tuple:
  case TypeKind::Option:
  case TypeKind::Result:
  case TypeKind::List:
  case TypeKind::Map:
  case TypeKind::IndexError:
  case TypeKind::ChannelError:
  case TypeKind::TaskError:
  case TypeKind::IoError:
  case TypeKind::TransportError:
  case TypeKind::ParseIntError:
  case TypeKind::Sender:
  case TypeKind::Receiver:
  case TypeKind::Task:
  case TypeKind::Listener:
  case TypeKind::Connection:
    return true;
  default:
    return false;
  }
}

bool TypeTable::supportsEquality(TypeId id) const {
  const auto& type = at(types, id);
  switch (type.kind) {
  case TypeKind::Unit:
  case TypeKind::Bool:
  case TypeKind::Int:
  case TypeKind::Float:
  case TypeKind::String:
  case TypeKind::Record:
  case TypeKind::Enum:
  case TypeKind::IndexError:
  case TypeKind::ChannelError:
  case TypeKind::TaskError:
  case TypeKind::IoError:
  case TypeKind::TransportError:
  case TypeKind::ParseIntError:
    return true;
  case TypeKind::Tuple:
  case TypeKind::Option:
  case TypeKind::Result:
  case TypeKind::List:
  case TypeKind::Map:
    return std::ranges::all_of(type.arguments, [&](TypeId argument) {
      return supportsEquality(argument);
    });
  default:
    return false;
  }
}

namespace sema::detail {

void TypeContext::initializeBuiltins() {
  table_.types = {
      {.kind = TypeKind::Unit, .arguments = {}, .declaration = {}},
      {.kind = TypeKind::Bool, .arguments = {}, .declaration = {}},
      {.kind = TypeKind::Int, .arguments = {}, .declaration = {}},
      {.kind = TypeKind::Float, .arguments = {}, .declaration = {}},
      {.kind = TypeKind::String, .arguments = {}, .declaration = {}},
      {.kind = TypeKind::Never, .arguments = {}, .declaration = {}},
      {.kind = TypeKind::Error, .arguments = {}, .declaration = {}},
      {.kind = TypeKind::Function, .arguments = {}, .declaration = {}},
      {.kind = TypeKind::IndexError, .arguments = {}, .declaration = {}}};
  table_.types.push_back(
      {.kind = TypeKind::ChannelError, .arguments = {}, .declaration = {}});
  table_.types.push_back(
      {.kind = TypeKind::TaskError, .arguments = {}, .declaration = {}});
  table_.types.push_back(
      {.kind = TypeKind::IoError, .arguments = {}, .declaration = {}});
  table_.types.push_back(
      {.kind = TypeKind::TransportError, .arguments = {}, .declaration = {}});
  table_.types.push_back(
      {.kind = TypeKind::ParseIntError, .arguments = {}, .declaration = {}});
}

TypeId TypeContext::intern(TypeKind kind, std::vector<TypeId> arguments,
                           std::optional<TypeDeclarationRef> declaration) {
  for (std::size_t index = 0; index < table_.types.size(); ++index) {
    const TypeId id{index};
    const auto& candidate = table_[id];
    if (candidate.kind == kind && candidate.arguments == arguments &&
        candidate.declaration == declaration)
      return id;
  }
  table_.types.push_back({.kind = kind,
                          .arguments = std::move(arguments),
                          .declaration = declaration});
  return TypeId{table_.types.size() - 1};
}

} // namespace sema::detail
} // namespace dola
