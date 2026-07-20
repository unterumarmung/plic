#include "dola/ast.h"

#include <sstream>

namespace dola {

std::string qualifiedName(const std::vector<std::string>& components) {
  std::ostringstream output;
  for (size_t i = 0; i < components.size(); ++i) {
    if (i)
      output << '.';
    output << components[i];
  }
  return output.str();
}

std::string primitiveTypeName(PrimitiveType type) {
  switch (type) {
  case PrimitiveType::Unit:
    return "Unit";
  case PrimitiveType::Bool:
    return "Bool";
  case PrimitiveType::Int:
    return "Int";
  case PrimitiveType::Float:
    return "Float";
  case PrimitiveType::String:
    return "String";
  }
  return "<invalid>";
}

std::string builtinTypeName(BuiltinType type) {
  switch (type) {
  case BuiltinType::Option:
    return "Option";
  case BuiltinType::Result:
    return "Result";
  case BuiltinType::List:
    return "List";
  case BuiltinType::Map:
    return "Map";
  case BuiltinType::Sender:
    return "Sender";
  case BuiltinType::Receiver:
    return "Receiver";
  case BuiltinType::Task:
    return "Task";
  case BuiltinType::Listener:
    return "Listener";
  case BuiltinType::Connection:
    return "Connection";
  }
  return "<invalid>";
}

} // namespace dola
