#include "dola/ast.h"

#include "dola/overloaded.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace dola {
namespace {

std::string escape(std::string_view value) {
  std::ostringstream output;
  output << '"';
  for (unsigned char byte : value) {
    switch (byte) {
    case '\\':
      output << "\\\\";
      break;
    case '"':
      output << "\\\"";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (byte < 0x20 || byte == 0x7f)
        output << "\\x" << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(byte) << std::dec;
      else
        output << static_cast<char>(byte);
    }
  }
  output << '"';
  return output.str();
}

class AstPrinter {
public:
  std::string print(const Module& module) {
    {
      auto scope = open("module " + qualifiedName(module.name));
      for (const auto& import : module.imports)
        leaf("use " + qualifiedName(import.moduleName));
      for (const auto& declaration : module.declarations) {
        std::visit(
            Overloaded{
                [&](FunctionIndex index) {
                  function(at(module.functions, index));
                },
                [&](RecordIndex index) { record(at(module.records, index)); },
                [&](EnumIndex index) { enumeration(at(module.enums, index)); }},
            declaration.index);
      }
    }
    return output_.str();
  }

private:
  class NodeScope {
  public:
    explicit NodeScope(AstPrinter& printer) : printer_(&printer) {}
    NodeScope(const NodeScope&) = delete;
    NodeScope& operator=(const NodeScope&) = delete;
    NodeScope(NodeScope&& other) noexcept
        : printer_(std::exchange(other.printer_, nullptr)) {}
    NodeScope& operator=(NodeScope&&) = delete;
    ~NodeScope() {
      if (printer_)
        printer_->closeNode();
    }

  private:
    AstPrinter* printer_;
  };

  void indent() {
    for (unsigned i = 0; i < depth_; ++i)
      output_ << "  ";
  }
  [[nodiscard]] NodeScope open(const std::string& text) {
    indent();
    output_ << '(' << text << '\n';
    ++depth_;
    return NodeScope(*this);
  }
  void closeNode() {
    --depth_;
    indent();
    output_ << ")\n";
  }
  void leaf(const std::string& text) {
    indent();
    output_ << text << '\n';
  }

  std::string type(const TypeRef& value) {
    return std::visit(
        Overloaded{[](const PrimitiveTypeRef& primitive) {
                     return primitiveTypeName(primitive.kind);
                   },
                   [](const NamedTypeRef& named) {
                     return qualifiedName(named.components);
                   },
                   [&](const TupleTypeRef& tuple) {
                     std::string result = "(";
                     for (size_t i = 0; i < tuple.elements.size(); ++i) {
                       if (i)
                         result += ", ";
                       result += type(*tuple.elements[i]);
                     }
                     return result + ")";
                   },
                   [&](const BuiltinTypeRef& builtin) {
                     std::string result = builtinTypeName(builtin.kind) + "[";
                     for (size_t i = 0; i < builtin.arguments.size(); ++i) {
                       if (i)
                         result += ", ";
                       result += type(*builtin.arguments[i]);
                     }
                     return result + "]";
                   }},
        value.value);
  }

  void function(const FunctionDecl& value) {
    auto scope = open(std::string(value.isPublic ? "pub-fn " : "fn ") +
                      value.name + " -> " + type(value.returnType));
    for (const auto& parameter : value.parameters)
      leaf("param " + parameter.name + ": " + type(parameter.type));
    block(*value.body);
  }
  void record(const RecordDecl& value) {
    auto scope = open(std::string(value.isPublic ? "pub-record " : "record ") +
                      value.name);
    for (const auto& field : value.fields)
      leaf("field " + field.name + ": " + type(field.type));
  }
  void enumeration(const EnumDecl& value) {
    auto scope =
        open(std::string(value.isPublic ? "pub-enum " : "enum ") + value.name);
    for (const auto& variant : value.variants) {
      if (variant.payloads.empty()) {
        leaf("variant " + variant.name);
        continue;
      }
      {
        auto variantScope = open("variant " + variant.name);
        for (const auto& payload : variant.payloads)
          leaf(type(payload));
      }
    }
  }

  void block(const Block& value) {
    auto scope = open("block");
    for (const auto& statement : value.statements)
      stmt(statement);
    if (value.tail) {
      auto tailScope = open("tail");
      expr(*value.tail);
    }
  }
  void stmt(const Statement& statement) {
    std::visit(
        Overloaded{
            [&](const BindingStmt& binding) {
              std::string target = std::visit(
                  Overloaded{[](const BindingName& name) { return name.name; },
                             [](const TupleBinding& tuple) {
                               std::string result = "(";
                               for (size_t i = 0; i < tuple.names.size(); ++i) {
                                 if (i)
                                   result += ", ";
                                 result += tuple.names[i];
                               }
                               return result + ")";
                             }},
                  binding.target);
              auto scope = open(
                  std::string(binding.mutableBinding ? "var " : "let ") +
                  target +
                  (binding.annotation ? ": " + type(*binding.annotation) : ""));
              expr(*binding.initializer);
            },
            [&](const AssignmentStmt& assignment) {
              auto scope = open("assign " + assignment.name);
              expr(*assignment.value);
            },
            [&](const ExpressionStmt& expression) {
              auto scope = open("expr-stmt");
              expr(*expression.value);
            }},
        statement.value);
  }

  void pattern(const Pattern& value) {
    std::visit(
        Overloaded{
            [&](const WildcardPattern&) { leaf("_"); },
            [&](const BindingPattern& binding) {
              leaf("bind " + binding.name);
            },
            [&](const LiteralPattern& literal) {
              std::visit(
                  Overloaded{
                      [&](bool item) { leaf(item ? "true" : "false"); },
                      [&](int64_t item) { leaf(std::to_string(item)); },
                      [&](const std::string& item) { leaf(escape(item)); }},
                  literal.value);
            },
            [&](const ConstructorPattern& constructor) {
              auto scope = open("pattern " + qualifiedName(constructor.name));
              for (const auto& argument : constructor.arguments)
                pattern(*argument);
            }},
        value.value);
  }

  void expr(const Expr& expression) {
    std::visit(
        Overloaded{
            [&](const LiteralExpr& literal) {
              std::visit(
                  Overloaded{
                      [&](const std::monostate&) { leaf("unit"); },
                      [&](bool value) { leaf(value ? "true" : "false"); },
                      [&](int64_t value) { leaf(std::to_string(value)); },
                      [&](double value) {
                        std::ostringstream text;
                        text << std::setprecision(17) << value;
                        leaf(text.str());
                      },
                      [&](const std::string& value) { leaf(escape(value)); }},
                  literal.value);
            },
            [&](const NameExpr& name) { leaf(qualifiedName(name.components)); },
            [&](const CallExpr& call) {
              auto scope = open("call");
              expr(*call.callee);
              for (const auto& argument : call.typeArguments)
                leaf("type " + type(argument));
              for (const auto& argument : call.arguments)
                expr(*argument);
            },
            [&](const SpawnExpr& spawn) {
              auto scope = open("spawn");
              expr(*spawn.call);
            },
            [&](const MemberExpr& member) {
              auto scope = open(
                  "member " +
                  std::visit(
                      Overloaded{[](const std::string& name) { return name; },
                                 [](TupleElementIndex index) {
                                   return std::to_string(index.value());
                                 }},
                      member.member));
              expr(*member.base);
            },
            [&](const UnaryExpr& unary) {
              auto scope = open("unary " + unary.op);
              expr(*unary.operand);
            },
            [&](const BinaryExpr& binary) {
              auto scope = open("binary " + binary.op);
              expr(*binary.left);
              expr(*binary.right);
            },
            [&](const IfExpr& value) {
              auto scope = open("if");
              expr(*value.condition);
              block(*value.thenBlock);
              std::visit(
                  Overloaded{[](const std::monostate&) {},
                             [&](const std::unique_ptr<Block>& branch) {
                               block(*branch);
                             },
                             [&](const ExprPtr& branch) { expr(*branch); }},
                  value.elseBranch);
            },
            [&](const BlockExpr& value) { block(*value.block); },
            [&](const TupleExpr& tuple) {
              auto scope = open("tuple");
              for (const auto& element : tuple.elements)
                expr(*element);
            },
            [&](const ListExpr& list) {
              auto scope = open("list");
              for (const auto& element : list.elements)
                expr(*element);
            },
            [&](const GenericConstructorExpr& constructor) {
              std::string text =
                  "new " + builtinTypeName(constructor.kind) + "[";
              for (size_t i = 0; i < constructor.arguments.size(); ++i) {
                if (i)
                  text += ", ";
                text += type(constructor.arguments[i]);
              }
              leaf(text + "]");
            },
            [&](const RecordExpr& record) {
              auto scope =
                  open("record-value " + qualifiedName(record.typeName));
              for (const auto& field : record.fields) {
                auto fieldScope = open("field " + field.name);
                expr(*field.value);
              }
            },
            [&](const RecordUpdateExpr& update) {
              auto scope = open("with");
              expr(*update.base);
              for (const auto& field : update.fields) {
                auto fieldScope = open("field " + field.name);
                expr(*field.value);
              }
            },
            [&](const TryExpr& value) {
              auto scope = open("try");
              expr(*value.value);
            },
            [&](const RangeExpr& range) {
              auto scope = open("range");
              expr(*range.start);
              expr(*range.end);
            },
            [&](const MatchExpr& match) {
              auto scope = open("match");
              expr(*match.value);
              for (const auto& arm : match.arms) {
                auto armScope = open("arm");
                pattern(*arm.pattern);
                expr(*arm.value);
              }
            },
            [&](const WhileExpr& loop) {
              auto scope = open("while");
              expr(*loop.condition);
              block(*loop.body);
            },
            [&](const ForExpr& loop) {
              auto scope = open("for " + loop.binding);
              expr(*loop.iterable);
              block(*loop.body);
            },
            [&](const LoopExpr& loop) {
              auto scope = open("loop");
              block(*loop.body);
            },
            [&](const ControlExpr& control) {
              std::string name = control.kind == ControlKind::Return ? "return"
                                 : control.kind == ControlKind::Break
                                     ? "break"
                                     : "continue";
              if (!control.value) {
                leaf(name);
                return;
              }
              auto scope = open(name);
              expr(*control.value);
            }},
        expression.value);
  }

  std::ostringstream output_;
  unsigned depth_ = 0;
};

} // namespace

std::string dumpAst(const Module& module) { return AstPrinter().print(module); }

} // namespace dola
