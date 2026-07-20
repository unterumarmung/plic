#include "dola/sema.h"

#include "dola/ast_analysis.h"
#include "dola/overloaded.h"

#include "module_graph.h"
#include "type_context.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>

namespace dola {

namespace {

using sema::detail::ModuleGraph;
using sema::detail::TypeContext;
using sema::detail::TypeDeclRef;

constexpr TypeId kUnit{0};
constexpr TypeId kBool{1};
constexpr TypeId kInt{2};
constexpr TypeId kFloat{3};
constexpr TypeId kString{4};
constexpr TypeId kNever{5};
constexpr TypeId kError{6};
constexpr TypeId kFunction{7};
constexpr TypeId kIndexError{8};
constexpr TypeId kChannelError{9};
constexpr TypeId kTaskError{10};
constexpr TypeId kIoError{11};
constexpr TypeId kTransportError{12};
constexpr TypeId kParseIntError{13};

struct Local {
  LocalId id;
  TypeId type;
  bool mutableBinding;
};

class Analyzer {
public:
  Analyzer(const std::vector<Module>& modules, DiagnosticEngine& diagnostics)
      : modules_(modules), diagnostics_(diagnostics),
        graph_(modules, diagnostics), types_(result_.typeTable) {
    types_.initializeBuiltins();
  }

  std::optional<SemanticResult> run(bool requireEntryPoint) {
    graph_.build();
    predeclareNamedTypes();
    resolveDeclarations();
    checkRecursiveTypes();
    for (std::size_t moduleIndex = 0; moduleIndex < modules_.size();
         ++moduleIndex) {
      const ModuleId module{moduleIndex};
      for (std::size_t functionIndex = 0;
           functionIndex < at(modules_, module).functions.size();
           ++functionIndex)
        checkFunction(module, FunctionIndex{functionIndex});
    }
    checkEntryPoint(requireEntryPoint);
    if (diagnostics_.hasErrors())
      return std::nullopt;
    return std::move(result_);
  }

private:
  [[nodiscard]] std::string typeName(TypeId type) const {
    return result_.typeTable.name(type, modules_);
  }

  void predeclareNamedTypes() {
    for (std::size_t moduleIndex = 0; moduleIndex < modules_.size();
         ++moduleIndex) {
      const ModuleId module{moduleIndex};
      for (std::size_t index = 0; index < at(modules_, module).records.size();
           ++index) {
        const RecordIndex record{index};
        const auto& declaration = at(at(modules_, module).records, record);
        TypeId type =
            types_.intern(TypeKind::Record, {},
                          RecordRef{.module = module, .record = record});
        result_.records.insert_or_assign(declaration.id,
                                         RecordTypeInfo{.type = type});
      }
      for (std::size_t index = 0; index < at(modules_, module).enums.size();
           ++index) {
        const EnumIndex enumeration{index};
        const auto& declaration = at(at(modules_, module).enums, enumeration);
        TypeId type = types_.intern(
            TypeKind::Enum, {},
            EnumRef{.module = module, .enumeration = enumeration});
        result_.enums.insert_or_assign(declaration.id,
                                       EnumTypeInfo{.type = type});
      }
    }
  }

  void resolveDeclarations() {
    for (std::size_t moduleIndex = 0; moduleIndex < modules_.size();
         ++moduleIndex) {
      const ModuleId module{moduleIndex};
      currentModule_ = module;
      for (const auto& record : at(modules_, module).records) {
        auto& info = result_.records.at(record.id);
        std::unordered_set<std::string> fields;
        for (const auto& field : record.fields) {
          if (!fields.insert(field.name).second)
            diagnostics_.error(field.location,
                               "duplicate record field `" + field.name + "`");
          info.fields.push_back(resolveType(field.type));
        }
      }
      for (const auto& enumeration : at(modules_, module).enums) {
        auto& info = result_.enums.at(enumeration.id);
        std::unordered_set<std::string> variants;
        for (const auto& variant : enumeration.variants) {
          if (!variants.insert(variant.name).second)
            diagnostics_.error(variant.location,
                               "duplicate enum variant `" + variant.name + "`");
          std::vector<TypeId> payloads;
          payloads.reserve(variant.payloads.size());
          for (const auto& payload : variant.payloads)
            payloads.push_back(resolveType(payload));
          info.variants.push_back(std::move(payloads));
        }
      }
      for (const auto& function : at(modules_, module).functions) {
        FunctionTypeInfo info;
        for (const auto& parameter : function.parameters)
          info.parameters.push_back(resolveType(parameter.type));
        info.result = resolveType(function.returnType);
        result_.functions.insert_or_assign(function.id, std::move(info));
      }
    }
  }

  TypeId resolveType(const TypeRef& syntax) {
    auto found = result_.resolvedTypes.find(&syntax);
    if (found != result_.resolvedTypes.end())
      return found->second;
    TypeId result = std::visit(
        Overloaded{
            [](const PrimitiveTypeRef& primitive) -> TypeId {
              switch (primitive.kind) {
              case PrimitiveType::Unit:
                return kUnit;
              case PrimitiveType::Bool:
                return kBool;
              case PrimitiveType::Int:
                return kInt;
              case PrimitiveType::Float:
                return kFloat;
              case PrimitiveType::String:
                return kString;
              }
              return kError;
            },
            [&](const NamedTypeRef& named) -> TypeId {
              if (named.components.size() == 1) {
                static const std::unordered_map<std::string, TypeId> builtins{
                    {"IndexError", kIndexError},
                    {"ChannelError", kChannelError},
                    {"TaskError", kTaskError},
                    {"IoError", kIoError},
                    {"TransportError", kTransportError},
                    {"ParseIntError", kParseIntError},
                };
                if (auto found = builtins.find(named.components.front());
                    found != builtins.end())
                  return found->second;
              }
              auto declaration =
                  resolveTypeDecl(named.components, syntax.location, true);
              if (!declaration)
                return kError;
              return std::visit(
                  Overloaded{
                      [&](RecordIndex record) {
                        return result_.records
                            .at(at(at(modules_, declaration->module).records,
                                   record)
                                    .id)
                            .type;
                      },
                      [&](EnumIndex enumeration) {
                        return result_.enums
                            .at(at(at(modules_, declaration->module).enums,
                                   enumeration)
                                    .id)
                            .type;
                      }},
                  declaration->declaration);
            },
            [&](const TupleTypeRef& tuple) -> TypeId {
              std::vector<TypeId> elements;
              elements.reserve(tuple.elements.size());
              for (const auto& element : tuple.elements)
                elements.push_back(resolveType(*element));
              if (elements.size() < 2 || elements.size() > 8) {
                diagnostics_.error(syntax.location,
                                   "tuple types require 2 to 8 elements");
                return kError;
              }
              return types_.intern(TypeKind::Tuple, std::move(elements));
            },
            [&](const BuiltinTypeRef& builtin) -> TypeId {
              std::vector<TypeId> arguments;
              arguments.reserve(builtin.arguments.size());
              for (const auto& argument : builtin.arguments)
                arguments.push_back(resolveType(*argument));
              const bool unary = builtin.kind == BuiltinType::Option ||
                                 builtin.kind == BuiltinType::List ||
                                 builtin.kind == BuiltinType::Sender ||
                                 builtin.kind == BuiltinType::Receiver ||
                                 builtin.kind == BuiltinType::Task;
              const size_t expected = unary ? 1 : 2;
              if (arguments.size() != expected) {
                diagnostics_.error(syntax.location,
                                   "`" + builtinTypeName(builtin.kind) +
                                       "` expects " + std::to_string(expected) +
                                       " type arguments");
                return kError;
              }
              TypeKind kind = TypeKind::Map;
              if (builtin.kind == BuiltinType::Option)
                kind = TypeKind::Option;
              else if (builtin.kind == BuiltinType::Result)
                kind = TypeKind::Result;
              else if (builtin.kind == BuiltinType::List)
                kind = TypeKind::List;
              else if (builtin.kind == BuiltinType::Sender)
                kind = TypeKind::Sender;
              else if (builtin.kind == BuiltinType::Receiver)
                kind = TypeKind::Receiver;
              else if (builtin.kind == BuiltinType::Task)
                kind = TypeKind::Task;
              else if (builtin.kind == BuiltinType::Listener)
                kind = TypeKind::Listener;
              else if (builtin.kind == BuiltinType::Connection)
                kind = TypeKind::Connection;
              if (kind == TypeKind::Map && arguments[0] != kInt &&
                  arguments[0] != kString)
                diagnostics_.error(syntax.location,
                                   "map key type must be `Int` or `String`");
              return types_.intern(kind, std::move(arguments));
            }},
        syntax.value);
    result_.resolvedTypes.insert_or_assign(&syntax, result);
    return result;
  }

  std::optional<TypeDeclRef>
  resolveTypeDecl(const std::vector<std::string>& name,
                  const SourceLocation& location, bool diagnose) {
    ModuleId module = currentModule_;
    std::string declaration;
    bool crossModule = false;
    if (name.size() == 1) {
      declaration = name.front();
    } else if (name.size() == 2) {
      const auto* imported = graph_.findImport(currentModule_, name.front());
      if (!imported || !*imported ||
          !std::holds_alternative<ModuleId>(**imported)) {
        if (diagnose)
          diagnostics_.error(location,
                             "unknown module qualifier `" + name.front() + "`");
        return std::nullopt;
      }
      module = std::get<ModuleId>(**imported);
      declaration = name[1];
      crossModule = true;
    } else {
      if (diagnose)
        diagnostics_.error(location,
                           "invalid type name `" + qualifiedName(name) + "`");
      return std::nullopt;
    }
    auto found = graph_.findType(module, declaration);
    if (!found) {
      if (diagnose)
        diagnostics_.error(location,
                           "unknown type `" + qualifiedName(name) + "`");
      return std::nullopt;
    }
    bool isPublic = std::visit(
        Overloaded{
            [&](RecordIndex record) {
              return at(at(modules_, module).records, record).isPublic;
            },
            [&](EnumIndex enumeration) {
              return at(at(modules_, module).enums, enumeration).isPublic;
            }},
        found->declaration);
    if (crossModule && !isPublic) {
      if (diagnose)
        diagnostics_.error(location,
                           "type `" + qualifiedName(name) + "` is private");
      return std::nullopt;
    }
    return found;
  }

  bool containsType(TypeId value, TypeId target,
                    std::unordered_set<TypeId>& visiting) const {
    if (value == target)
      return true;
    if (!visiting.insert(value).second)
      return false;
    const auto& type = result_.typeTable[value];
    for (TypeId argument : type.arguments)
      if (containsType(argument, target, visiting))
        return true;
    if (type.kind == TypeKind::Record) {
      if (!type.declaration)
        return false;
      const auto reference = std::get<RecordRef>(type.declaration.value());
      const auto& declaration =
          at(at(modules_, reference.module).records, reference.record);
      for (TypeId field : result_.records.at(declaration.id).fields)
        if (containsType(field, target, visiting))
          return true;
    } else if (type.kind == TypeKind::Enum) {
      if (!type.declaration)
        return false;
      const auto reference = std::get<EnumRef>(type.declaration.value());
      const auto& declaration =
          at(at(modules_, reference.module).enums, reference.enumeration);
      for (const auto& variant : result_.enums.at(declaration.id).variants)
        for (TypeId payload : variant)
          if (containsType(payload, target, visiting))
            return true;
    }
    return false;
  }

  void checkRecursiveTypes() {
    for (const auto& [id, info] : result_.records) {
      std::unordered_set<TypeId> visiting{info.type};
      for (TypeId field : info.fields)
        if (containsType(field, info.type, visiting)) {
          diagnostics_.error(findRecord(id).location,
                             "recursive record types are not supported");
          break;
        }
    }
    for (const auto& [id, info] : result_.enums) {
      std::unordered_set<TypeId> visiting{info.type};
      bool recursive = false;
      for (const auto& variant : info.variants)
        for (TypeId payload : variant)
          recursive |= containsType(payload, info.type, visiting);
      if (recursive)
        diagnostics_.error(findEnum(id).location,
                           "recursive enum types are not supported");
    }
  }

  [[nodiscard]] const RecordDecl& findRecord(DeclId id) const {
    for (const auto& module : modules_)
      for (const auto& record : module.records)
        if (record.id == id)
          return record;
    std::abort();
  }
  [[nodiscard]] const EnumDecl& findEnum(DeclId id) const {
    for (const auto& module : modules_)
      for (const auto& enumeration : module.enums)
        if (enumeration.id == id)
          return enumeration;
    std::abort();
  }

  void checkEntryPoint(bool required) {
    std::vector<FunctionRef> entries = graph_.functionsNamed("main");
    if (entries.empty()) {
      if (required)
        diagnostics_.error(modules_.empty() ? SourceLocation{.path = "<input>"}
                                            : modules_.front().location,
                           "executable requires exactly one `main` function");
      return;
    }
    if (entries.size() != 1) {
      diagnostics_.error(
          at(at(modules_, entries[1].module).functions, entries[1].function)
              .location,
          "executable has more than one `main` function");
      return;
    }
    const auto& main =
        at(at(modules_, entries[0].module).functions, entries[0].function);
    const auto& type = result_.functions.at(main.id);
    if (!main.parameters.empty())
      diagnostics_.error(main.location, "`main` must not have parameters");
    bool validResult = type.result == kUnit || type.result == kInt;
    if (result_.typeTable[type.result].kind == TypeKind::Result) {
      const auto& arguments = result_.typeTable[type.result].arguments;
      validResult = arguments[0] == kUnit && arguments[1] == kString;
    }
    if (!validResult)
      diagnostics_.error(
          main.location,
          "`main` must return `Unit`, `Int`, or `Result[Unit, String]`");
    result_.entryPoint = FunctionRef{.module = entries[0].module,
                                     .function = entries[0].function};
  }

  void checkFunction(ModuleId moduleIndex, FunctionIndex functionIndex) {
    currentModule_ = moduleIndex;
    currentFunction_ = &at(at(modules_, moduleIndex).functions, functionIndex);
    currentReturn_ = result_.functions.at(currentFunction_->id).result;
    scopes_.clear();
    scopes_.emplace_back();
    nextLocal_ = LocalId{0};
    const auto& type = result_.functions.at(currentFunction_->id);
    for (size_t i = 0; i < currentFunction_->parameters.size(); ++i) {
      const auto& parameter = currentFunction_->parameters[i];
      if (scopes_.back().contains(parameter.name))
        diagnostics_.error(parameter.location,
                           "duplicate parameter `" + parameter.name + "`");
      else {
        scopes_.back().insert_or_assign(parameter.name,
                                        Local{.id = nextLocal_,
                                              .type = type.parameters.at(i),
                                              .mutableBinding = false});
        nextLocal_ = LocalId{nextLocal_.value() + 1};
      }
    }
    TypeId bodyType =
        checkBlock(*currentFunction_->body, false, currentReturn_);
    if (!blockDefinitelyReturns(*currentFunction_->body) &&
        bodyType != kError && bodyType != kNever && bodyType != currentReturn_)
      diagnostics_.error(currentFunction_->body->location,
                         "expected function body to produce `" +
                             typeName(currentReturn_) + "`, found `" +
                             typeName(bodyType) + "`");
  }

  TypeId checkBlock(const Block& block, bool nested,
                    std::optional<TypeId> expected = std::nullopt) {
    if (nested)
      scopes_.emplace_back();
    for (const auto& statement : block.statements)
      checkStatement(statement);
    TypeId result = block.tail
                        ? checkExpr(*block.tail, expected)
                        : (blockDefinitelyReturns(block) ? kNever : kUnit);
    if (nested)
      scopes_.pop_back();
    return result;
  }

  void checkLoopBody(const Block& block, bool nested) {
    const TypeId result = checkBlock(block, nested, kUnit);
    if (result != kUnit && result != kNever && result != kError)
      typeMismatch(block.tail ? block.tail->location : block.location, kUnit,
                   result);
  }

  void checkStatement(const Statement& statement) {
    std::visit(Overloaded{[&](const BindingStmt& binding) {
                            checkBinding(statement, binding);
                          },
                          [&](const AssignmentStmt& assignment) {
                            checkAssignment(statement, assignment);
                          },
                          [&](const ExpressionStmt& expression) {
                            checkExpr(*expression.value);
                          }},
               statement.value);
  }

  void addLocal(const SourceLocation& location, const std::string& name,
                TypeId type, bool mutableBinding, std::vector<LocalId>& ids) {
    if (scopes_.back().contains(name)) {
      diagnostics_.error(location, "duplicate local binding `" + name + "`");
      return;
    }
    const LocalId id = nextLocal_;
    nextLocal_ = LocalId{nextLocal_.value() + 1};
    scopes_.back().insert_or_assign(
        name, Local{.id = id, .type = type, .mutableBinding = mutableBinding});
    ids.push_back(id);
  }

  void checkBinding(const Statement& statement, const BindingStmt& binding) {
    std::optional<TypeId> declared;
    if (binding.annotation)
      declared = resolveType(*binding.annotation);
    TypeId initializer = checkExpr(*binding.initializer, declared);
    if (declared && initializer != kError && initializer != kNever &&
        initializer != *declared)
      typeMismatch(binding.initializer->location, *declared, initializer);
    TypeId type = declared.value_or(initializer);
    std::vector<LocalId> ids;
    std::visit(
        Overloaded{
            [&](const BindingName& name) {
              addLocal(statement.location, name.name, type,
                       binding.mutableBinding, ids);
            },
            [&](const TupleBinding& tuple) {
              if (binding.mutableBinding)
                diagnostics_.error(statement.location,
                                   "tuple destructuring requires `let`");
              if (result_.typeTable[type].kind != TypeKind::Tuple) {
                diagnostics_.error(statement.location,
                                   "tuple binding requires a tuple value");
                return;
              }
              const auto& elements = result_.typeTable[type].arguments;
              if (elements.size() != tuple.names.size()) {
                diagnostics_.error(statement.location,
                                   "tuple binding arity does not match value");
                return;
              }
              for (size_t i = 0; i < tuple.names.size(); ++i)
                addLocal(statement.location, tuple.names[i], elements[i], false,
                         ids);
            }},
        binding.target);
    result_.bindings.insert_or_assign(&statement, std::move(ids));
  }

  void checkAssignment(const Statement& statement,
                       const AssignmentStmt& assignment) {
    auto local = findLocal(assignment.name);
    TypeId assigned = checkExpr(
        *assignment.value, local ? std::optional(local->type) : std::nullopt);
    if (!local) {
      diagnostics_.error(statement.location,
                         "unresolved name `" + assignment.name + "`");
      return;
    }
    if (!local->mutableBinding)
      diagnostics_.error(statement.location,
                         "cannot assign to immutable binding `" +
                             assignment.name + "`");
    if (assigned != kError && assigned != kNever && assigned != local->type)
      typeMismatch(assignment.value->location, local->type, assigned);
    result_.assignments.insert_or_assign(&statement, local->id);
  }

  [[nodiscard]] std::optional<Local> findLocal(const std::string& name) const {
    for (const auto& scope : std::views::reverse(scopes_)) {
      auto found = scope.find(name);
      if (found != scope.end())
        return found->second;
    }
    return std::nullopt;
  }

  void typeMismatch(const SourceLocation& location, TypeId expected,
                    TypeId found) {
    diagnostics_.error(location, "expected `" + typeName(expected) +
                                     "`, found `" + typeName(found) + "`");
  }

  [[nodiscard]] std::optional<std::vector<std::string>>
  path(const Expr& expression) const {
    if (const auto* name = std::get_if<NameExpr>(&expression.value))
      return name->components;
    if (const auto* member = std::get_if<MemberExpr>(&expression.value)) {
      const auto* text = std::get_if<std::string>(&member->member);
      if (!text)
        return std::nullopt;
      auto base = path(*member->base);
      if (!base)
        return std::nullopt;
      base->push_back(*text);
      return base;
    }
    return std::nullopt;
  }

  TypeId checkExpr(const Expr& expression,
                   std::optional<TypeId> expected = std::nullopt) {
    TypeId type = std::visit(
        Overloaded{
            [&](const LiteralExpr& value) -> TypeId {
              return std::visit(
                  Overloaded{
                      [](const std::monostate&) -> TypeId { return kUnit; },
                      [](bool) -> TypeId { return kBool; },
                      [](int64_t) -> TypeId { return kInt; },
                      [](double) -> TypeId { return kFloat; },
                      [](const std::string&) -> TypeId { return kString; }},
                  value.value);
            },
            [&](const NameExpr& value) -> TypeId {
              if (auto local = findLocal(value.components.front())) {
                result_.references.insert_or_assign(expression.id,
                                                    LocalReference{local->id});
                return local->type;
              }
              if (value.components.front() == "None") {
                if (!expected ||
                    result_.typeTable[*expected].kind != TypeKind::Option) {
                  diagnostics_.error(
                      expression.location,
                      "`None` requires an expected `Option` type");
                  return kError;
                }
                result_.references.insert_or_assign(
                    expression.id, OptionConstructorReference{false});
                return *expected;
              }
              diagnostics_.error(expression.location,
                                 "unresolved name `" +
                                     qualifiedName(value.components) + "`");
              return kError;
            },
            [&](const MemberExpr& value) -> TypeId {
              if (auto fullPath = path(expression)) {
                if (auto standard =
                        resolveStandardErrorValue(*fullPath, expression, false))
                  return *standard;
                if (auto variant = resolveVariantValue(*fullPath, expression,
                                                       expected, false))
                  return *variant;
              }
              TypeId base = checkExpr(*value.base);
              if (base == kError)
                return kError;
              if (const auto* index =
                      std::get_if<TupleElementIndex>(&value.member)) {
                if (result_.typeTable[base].kind != TypeKind::Tuple ||
                    index->value() >=
                        result_.typeTable[base].arguments.size()) {
                  diagnostics_.error(expression.location,
                                     "tuple field index is out of range");
                  return kError;
                }
                result_.references.insert_or_assign(
                    expression.id, TupleFieldReference{*index});
                return result_.typeTable[base].arguments.at(index->value());
              }
              const auto& semantic = result_.typeTable[base];
              if (semantic.kind != TypeKind::Record) {
                diagnostics_.error(expression.location,
                                   "field access requires a record value");
                return kError;
              }
              if (!semantic.declaration)
                return kError;
              const RecordRef record =
                  std::get<RecordRef>(semantic.declaration.value());
              const auto& declaration =
                  at(at(modules_, record.module).records, record.record);
              const auto& info = result_.records.at(declaration.id);
              const auto& field = std::get<std::string>(value.member);
              for (std::size_t i = 0; i < declaration.fields.size(); ++i)
                if (declaration.fields.at(i).name == field) {
                  result_.references.insert_or_assign(
                      expression.id,
                      RecordFieldReference{.record = record,
                                           .field = FieldIndex{i}});
                  return info.fields.at(i);
                }
              diagnostics_.error(expression.location,
                                 "unknown record field `" + field + "`");
              return kError;
            },
            [&](const CallExpr& value) -> TypeId {
              return checkCall(expression, value, expected);
            },
            [&](const SpawnExpr& value) -> TypeId {
              const auto* call = std::get_if<CallExpr>(&value.call->value);
              if (!call) {
                diagnostics_.error(expression.location,
                                   "`spawn` requires a direct function call");
                checkExpr(*value.call);
                return kError;
              }
              auto calleePath = path(*call->callee);
              if (!calleePath) {
                diagnostics_.error(call->callee->location,
                                   "spawn target must be a direct function");
                checkCall(*value.call, *call, std::nullopt);
                return kError;
              }
              auto function =
                  resolveFunction(*calleePath, call->callee->location, true);
              if (!function) {
                for (const auto& argument : call->arguments)
                  checkExpr(*argument);
                return kError;
              }
              TypeId result = checkCall(*value.call, *call, std::nullopt);
              result_.references.insert_or_assign(expression.id,
                                                  SpawnReference{*function});
              return types_.intern(TypeKind::Task, {result});
            },
            [&](const UnaryExpr& value) -> TypeId {
              TypeId operand = checkExpr(*value.operand);
              if (value.op == "!") {
                if (operand != kBool && operand != kError)
                  diagnostics_.error(expression.location,
                                     "operator `!` requires `Bool`");
                return kBool;
              }
              if (operand != kInt && operand != kFloat && operand != kError)
                diagnostics_.error(expression.location,
                                   "unary `-` requires a numeric operand");
              return operand;
            },
            [&](const BinaryExpr& value) -> TypeId {
              return checkBinary(expression, value);
            },
            [&](const IfExpr& value) -> TypeId {
              TypeId condition = checkExpr(*value.condition, kBool);
              if (condition != kBool && condition != kError)
                diagnostics_.error(value.condition->location,
                                   "`if` condition must be `Bool`");
              TypeId thenType = checkBlock(*value.thenBlock, true, expected);
              return std::visit(
                  Overloaded{[&](const std::monostate&) -> TypeId {
                               if (thenType != kUnit && thenType != kNever &&
                                   thenType != kError)
                                 diagnostics_.error(
                                     expression.location,
                                     "`if` without `else` must produce `Unit`");
                               return kUnit;
                             },
                             [&](const std::unique_ptr<Block>& branch) {
                               return mergeBranches(
                                   expression.location, thenType,
                                   checkBlock(*branch, true, expected));
                             },
                             [&](const ExprPtr& branch) {
                               return mergeBranches(
                                   expression.location, thenType,
                                   checkExpr(*branch, expected));
                             }},
                  value.elseBranch);
            },
            [&](const BlockExpr& value) -> TypeId {
              return checkBlock(*value.block, true, expected);
            },
            [&](const TupleExpr& value) -> TypeId {
              if (value.elements.size() < 2 || value.elements.size() > 8) {
                diagnostics_.error(expression.location,
                                   "tuple values require 2 to 8 elements");
                return kError;
              }
              const SemanticType* expectedTuple =
                  expected &&
                          result_.typeTable[*expected].kind == TypeKind::Tuple
                      ? &result_.typeTable[*expected]
                      : nullptr;
              std::vector<TypeId> elements;
              elements.reserve(value.elements.size());
              for (size_t i = 0; i < value.elements.size(); ++i)
                elements.push_back(checkExpr(
                    *value.elements[i],
                    expectedTuple && i < expectedTuple->arguments.size()
                        ? std::optional(expectedTuple->arguments[i])
                        : std::nullopt));
              return types_.intern(TypeKind::Tuple, std::move(elements));
            },
            [&](const ListExpr& value) -> TypeId {
              std::optional<TypeId> element;
              if (expected &&
                  result_.typeTable[*expected].kind == TypeKind::List)
                element = result_.typeTable[*expected].arguments.front();
              if (value.elements.empty() && !element) {
                diagnostics_.error(
                    expression.location,
                    "empty list requires an expected `List` type");
                return kError;
              }
              for (const auto& item : value.elements) {
                TypeId itemType = checkExpr(*item, element);
                if (!element)
                  element = itemType;
                else if (itemType != kError && itemType != kNever &&
                         itemType != *element)
                  typeMismatch(item->location, *element, itemType);
              }
              return types_.intern(TypeKind::List, {*element});
            },
            [&](const GenericConstructorExpr& value) -> TypeId {
              std::vector<TypeId> arguments;
              arguments.reserve(value.arguments.size());
              for (const auto& argument : value.arguments)
                arguments.push_back(resolveType(argument));
              TypeKind kind = value.kind == BuiltinType::List ? TypeKind::List
                                                              : TypeKind::Map;
              if ((kind == TypeKind::List && arguments.size() != 1) ||
                  (kind == TypeKind::Map && arguments.size() != 2)) {
                diagnostics_.error(expression.location,
                                   "invalid collection type argument count");
                return kError;
              }
              if (kind == TypeKind::Map && arguments[0] != kInt &&
                  arguments[0] != kString)
                diagnostics_.error(expression.location,
                                   "map key type must be `Int` or `String`");
              return types_.intern(kind, std::move(arguments));
            },
            [&](const RecordExpr& value) -> TypeId {
              return checkRecord(expression, value);
            },
            [&](const RecordUpdateExpr& value) -> TypeId {
              return checkRecordUpdate(expression, value);
            },
            [&](const TryExpr& value) -> TypeId {
              TypeId operand = checkExpr(*value.value);
              if (result_.typeTable[operand].kind != TypeKind::Result) {
                diagnostics_.error(expression.location,
                                   "postfix `?` requires a `Result` value");
                return kError;
              }
              if (result_.typeTable[currentReturn_].kind != TypeKind::Result ||
                  result_.typeTable[operand].arguments[1] !=
                      result_.typeTable[currentReturn_].arguments[1]) {
                diagnostics_.error(
                    expression.location,
                    "postfix `?` error type must match the function result");
                return kError;
              }
              return result_.typeTable[operand].arguments[0];
            },
            [&](const RangeExpr& value) -> TypeId {
              TypeId start = checkExpr(*value.start, kInt);
              TypeId end = checkExpr(*value.end, kInt);
              if ((start != kInt && start != kError) ||
                  (end != kInt && end != kError))
                diagnostics_.error(expression.location,
                                   "range bounds must be `Int`");
              return types_.intern(TypeKind::Range, {kInt});
            },
            [&](const MatchExpr& value) -> TypeId {
              return checkMatch(expression, value, expected);
            },
            [&](const WhileExpr& value) -> TypeId {
              TypeId condition = checkExpr(*value.condition, kBool);
              if (condition != kBool && condition != kError)
                diagnostics_.error(value.condition->location,
                                   "`while` condition must be `Bool`");
              ++loopDepth_;
              checkLoopBody(*value.body, true);
              --loopDepth_;
              return kUnit;
            },
            [&](const ForExpr& value) -> TypeId {
              TypeId iterable = checkExpr(*value.iterable);
              TypeId element = kError;
              if (result_.typeTable[iterable].kind == TypeKind::List ||
                  result_.typeTable[iterable].kind == TypeKind::Range)
                element = result_.typeTable[iterable].arguments.front();
              else
                diagnostics_.error(value.iterable->location,
                                   "`for` requires a list or integer range");
              scopes_.emplace_back();
              const LocalId id = nextLocal_;
              nextLocal_ = LocalId{nextLocal_.value() + 1};
              scopes_.back().insert_or_assign(
                  value.binding,
                  Local{.id = id, .type = element, .mutableBinding = false});
              result_.loopBindings.insert_or_assign(expression.id, id);
              ++loopDepth_;
              checkLoopBody(*value.body, false);
              --loopDepth_;
              scopes_.pop_back();
              return kUnit;
            },
            [&](const LoopExpr& value) -> TypeId {
              ++loopDepth_;
              checkLoopBody(*value.body, true);
              --loopDepth_;
              return kUnit;
            },
            [&](const ControlExpr& value) -> TypeId {
              if (value.kind == ControlKind::Return) {
                TypeId returned = value.value
                                      ? checkExpr(*value.value, currentReturn_)
                                      : kUnit;
                if (returned != kError && returned != kNever &&
                    returned != currentReturn_)
                  typeMismatch(expression.location, currentReturn_, returned);
              } else if (loopDepth_ == 0) {
                diagnostics_.error(
                    expression.location,
                    value.kind == ControlKind::Break
                        ? "`break` is only valid inside a loop"
                        : "`continue` is only valid inside a loop");
              }
              return kNever;
            }},
        expression.value);
    result_.expressionTypes.insert_or_assign(expression.id, type);
    return type;
  }

  TypeId mergeBranches(const SourceLocation& location, TypeId left,
                       TypeId right) {
    if (left == kNever)
      return right;
    if (right == kNever)
      return left;
    if (left == kError || right == kError)
      return kError;
    if (left != right) {
      diagnostics_.error(
          location, "branches must produce the same type, found `" +
                        typeName(left) + "` and `" + typeName(right) + "`");
      return kError;
    }
    return left;
  }

  TypeId checkBinary(const Expr& expression, const BinaryExpr& value) {
    TypeId left = checkExpr(*value.left), right = checkExpr(*value.right, left);
    if (left == kError || right == kError)
      return kError;
    if (left != right) {
      diagnostics_.error(expression.location,
                         "operator operands must have matching types");
      return kError;
    }
    if (value.op == "&&" || value.op == "||") {
      if (left != kBool)
        diagnostics_.error(expression.location,
                           "operator `" + value.op +
                               "` requires `Bool` operands");
      return kBool;
    }
    if (value.op == "==" || value.op == "!=") {
      if (!result_.typeTable.supportsEquality(left))
        diagnostics_.error(expression.location,
                           "equality is not implemented for `" +
                               typeName(left) + "`");
      return kBool;
    }
    if (value.op == "<" || value.op == "<=" || value.op == ">" ||
        value.op == ">=") {
      if (left != kInt && left != kFloat)
        diagnostics_.error(expression.location,
                           "comparison requires numeric operands");
      return kBool;
    }
    if (value.op == "+" && left == kString)
      return kString;
    if (left != kInt && left != kFloat)
      diagnostics_.error(expression.location,
                         "arithmetic requires numeric operands");
    return left;
  }

  std::optional<FunctionRef>
  resolveFunction(const std::vector<std::string>& name,
                  const SourceLocation& location, bool diagnose) {
    ModuleId module = currentModule_;
    std::string function;
    bool crossModule = false;
    if (name.size() == 1)
      function = name[0];
    else if (name.size() == 2) {
      const auto* imported = graph_.findImport(currentModule_, name[0]);
      if (!imported || !*imported ||
          !std::holds_alternative<ModuleId>(**imported)) {
        if (diagnose)
          diagnostics_.error(location,
                             "unknown module qualifier `" + name[0] + "`");
        return std::nullopt;
      }
      module = std::get<ModuleId>(**imported);
      function = name[1];
      crossModule = true;
    } else
      return std::nullopt;
    auto found = graph_.findFunction(module, function);
    if (!found) {
      if (diagnose)
        diagnostics_.error(location,
                           "unknown function `" + qualifiedName(name) + "`");
      return std::nullopt;
    }
    const auto& declaration =
        at(at(modules_, module).functions, found->function);
    if (crossModule && !declaration.isPublic) {
      if (diagnose)
        diagnostics_.error(location,
                           "function `" + qualifiedName(name) + "` is private");
      return std::nullopt;
    }
    return found;
  }

  TypeId checkCall(const Expr& expression, const CallExpr& value,
                   std::optional<TypeId> expected) {
    auto calleePath = path(*value.callee);
    auto builtinCall = [&](BuiltinFunction function,
                           const std::vector<TypeId>& parameters, TypeId result,
                           std::string_view name) {
      if (value.arguments.size() != parameters.size())
        diagnostics_.error(expression.location,
                           "`" + std::string(name) + "` expects " +
                               std::to_string(parameters.size()) +
                               " arguments");
      for (std::size_t index = 0; index < value.arguments.size(); ++index) {
        TypeId argument = checkExpr(*value.arguments[index],
                                    index < parameters.size()
                                        ? std::optional(parameters[index])
                                        : std::nullopt);
        if (index < parameters.size() && argument != kError &&
            argument != kNever && argument != parameters[index])
          typeMismatch(value.arguments[index]->location, parameters[index],
                       argument);
      }
      result_.references.insert_or_assign(value.callee->id,
                                          BuiltinFunctionReference{function});
      result_.expressionTypes.insert_or_assign(value.callee->id, kFunction);
      return result;
    };
    if (calleePath && calleePath->size() == 1 &&
        calleePath->front() == "channel") {
      if (value.typeArguments.size() != 1)
        diagnostics_.error(expression.location,
                           "`channel` expects 1 type argument");
      if (!value.arguments.empty())
        diagnostics_.error(expression.location,
                           "`channel` expects 0 arguments");
      std::vector<TypeId> arguments;
      arguments.reserve(value.typeArguments.size());
      for (const auto& argument : value.typeArguments)
        arguments.push_back(resolveType(argument));
      if (arguments.size() != 1)
        return kError;
      TypeId sender = types_.intern(TypeKind::Sender, {arguments.front()});
      TypeId receiver = types_.intern(TypeKind::Receiver, {arguments.front()});
      result_.references.insert_or_assign(
          value.callee->id,
          BuiltinFunctionReference{BuiltinFunction::ChannelCreate});
      result_.expressionTypes.insert_or_assign(value.callee->id, kFunction);
      return types_.intern(TypeKind::Tuple, {sender, receiver});
    }
    if (calleePath && calleePath->size() == 1 &&
        calleePath->front() == "panic") {
      if (!value.typeArguments.empty())
        diagnostics_.error(expression.location,
                           "`panic` does not accept type arguments");
      if (value.arguments.size() != 1)
        diagnostics_.error(expression.location, "`panic` expects 1 argument");
      for (const auto& argument : value.arguments) {
        TypeId type = checkExpr(*argument, kString);
        if (type != kString && type != kError)
          typeMismatch(argument->location, kString, type);
      }
      result_.references.insert_or_assign(
          value.callee->id, BuiltinFunctionReference{BuiltinFunction::Panic});
      result_.expressionTypes.insert_or_assign(value.callee->id, kFunction);
      return kNever;
    }
    if (calleePath && *calleePath == std::vector<std::string>{"Int", "parse"}) {
      if (!value.typeArguments.empty())
        diagnostics_.error(expression.location,
                           "`Int.parse` does not accept type arguments");
      return builtinCall(
          BuiltinFunction::ParseInt, {kString},
          types_.intern(TypeKind::Result, {kInt, kParseIntError}), "Int.parse");
    }
    if (calleePath && calleePath->size() == 2 &&
        calleePath->front() == "time") {
      if (!graph_.hasBuiltinImport(currentModule_, "time",
                                   ModuleGraph::BuiltinModule::Time))
        diagnostics_.error(value.callee->location,
                           "`std.time` is not imported");
      if (!value.typeArguments.empty())
        diagnostics_.error(expression.location,
                           "`std.time` functions do not accept type arguments");
      if (calleePath->back() == "unix_seconds")
        return builtinCall(BuiltinFunction::UnixSeconds, {}, kInt,
                           "time.unix_seconds");
      if (calleePath->back() == "sleep_ms")
        return builtinCall(BuiltinFunction::SleepMilliseconds, {kInt}, kUnit,
                           "time.sleep_ms");
    }
    if (calleePath && calleePath->size() == 2 &&
        calleePath->front() == "transport" &&
        (calleePath->back() == "listen" || calleePath->back() == "connect")) {
      if (!graph_.hasBuiltinImport(currentModule_, "transport",
                                   ModuleGraph::BuiltinModule::Transport))
        diagnostics_.error(value.callee->location,
                           "`std.transport` is not imported");
      if (value.typeArguments.size() != 2)
        diagnostics_.error(expression.location,
                           "`transport." + calleePath->back() +
                               "` expects 2 type arguments");
      std::vector<TypeId> types;
      for (const auto& argument : value.typeArguments) {
        TypeId resolved = resolveType(argument);
        types.push_back(resolved);
        if (auto rejected = nonSerializableType(resolved))
          diagnostics_.error(argument.location,
                             "transport message type is not serializable "
                             "because `" +
                                 typeName(*rejected) +
                                 "` is a runtime resource");
      }
      if (types.size() != 2) {
        builtinCall(calleePath->back() == "listen"
                        ? BuiltinFunction::TransportListen
                        : BuiltinFunction::TransportConnect,
                    {kString}, kError, "transport." + calleePath->back());
        return kError;
      }
      TypeKind handle = calleePath->back() == "listen" ? TypeKind::Listener
                                                       : TypeKind::Connection;
      TypeId resource = types_.intern(handle, types);
      return builtinCall(
          calleePath->back() == "listen" ? BuiltinFunction::TransportListen
                                         : BuiltinFunction::TransportConnect,
          {kString},
          types_.intern(TypeKind::Result, {resource, kTransportError}),
          "transport." + calleePath->back());
    }
    if (calleePath && calleePath->size() == 1 &&
        ((*calleePath)[0] == "Some" || (*calleePath)[0] == "Ok" ||
         (*calleePath)[0] == "Err"))
      return checkBuiltinConstructor(expression, value, *calleePath, expected);
    if (calleePath) {
      if (auto standard =
              resolveStandardErrorCall(expression, value, *calleePath))
        return *standard;
      if (auto variant =
              resolveVariantCall(expression, value, *calleePath, expected))
        return *variant;
    }
    if (calleePath && calleePath->size() == 2 && (*calleePath)[0] == "io") {
      if (!graph_.hasBuiltinImport(currentModule_, "io",
                                   ModuleGraph::BuiltinModule::Io))
        diagnostics_.error(value.callee->location, "`std.io` is not imported");
      if (!value.typeArguments.empty())
        diagnostics_.error(expression.location,
                           "`std.io` functions do not accept type arguments");
      if ((*calleePath)[1] == "print" || (*calleePath)[1] == "println") {
        const bool newline = (*calleePath)[1] == "println";
        TypeId result = builtinCall(newline ? BuiltinFunction::Println
                                            : BuiltinFunction::Print,
                                    {kString}, kUnit, "io." + (*calleePath)[1]);
        result_.references.insert_or_assign(value.callee->id,
                                            OutputReference{newline});
        return result;
      }
      if ((*calleePath)[1] == "read_line")
        return builtinCall(BuiltinFunction::ReadLine, {},
                           types_.intern(TypeKind::Result, {kString, kIoError}),
                           "io.read_line");
      if ((*calleePath)[1] == "read_text")
        return builtinCall(BuiltinFunction::ReadText, {kString},
                           types_.intern(TypeKind::Result, {kString, kIoError}),
                           "io.read_text");
      if ((*calleePath)[1] == "write_text")
        return builtinCall(BuiltinFunction::WriteText, {kString, kString},
                           types_.intern(TypeKind::Result, {kUnit, kIoError}),
                           "io.write_text");
    }
    if (const auto* member = std::get_if<MemberExpr>(&value.callee->value)) {
      if (const auto* method = std::get_if<std::string>(&member->member)) {
        if (!calleePath ||
            !resolveFunction(*calleePath, value.callee->location, false))
          return checkMethod(expression, value, *member, *method);
      }
    }
    if (!calleePath) {
      diagnostics_.error(value.callee->location,
                         "call target must be a direct function or method");
      return kError;
    }
    auto function = resolveFunction(*calleePath, value.callee->location, true);
    if (!function)
      return kError;
    if (!value.typeArguments.empty())
      diagnostics_.error(expression.location,
                         "user functions do not accept type arguments");
    const auto& declaration =
        at(at(modules_, function->module).functions, function->function);
    const auto& type = result_.functions.at(declaration.id);
    result_.references.insert_or_assign(
        value.callee->id,
        FunctionReference{FunctionRef{.module = function->module,
                                      .function = function->function}});
    result_.expressionTypes.insert_or_assign(value.callee->id, kFunction);
    if (value.arguments.size() != type.parameters.size())
      diagnostics_.error(expression.location,
                         "function `" + declaration.name + "` expects " +
                             std::to_string(type.parameters.size()) +
                             " arguments, found " +
                             std::to_string(value.arguments.size()));
    for (size_t i = 0; i < value.arguments.size(); ++i) {
      TypeId argument =
          checkExpr(*value.arguments[i], i < type.parameters.size()
                                             ? std::optional(type.parameters[i])
                                             : std::nullopt);
      if (i < type.parameters.size() && argument != kError &&
          argument != kNever && argument != type.parameters[i])
        typeMismatch(value.arguments[i]->location, type.parameters[i],
                     argument);
    }
    return type.result;
  }

  TypeId checkBuiltinConstructor(const Expr& expression, const CallExpr& value,
                                 const std::vector<std::string>& name,
                                 std::optional<TypeId> expected) {
    if (value.arguments.size() != 1) {
      diagnostics_.error(expression.location,
                         "`" + name.front() + "` expects 1 argument");
      return kError;
    }
    if (name.front() == "Some") {
      std::optional<TypeId> element;
      if (expected && result_.typeTable[*expected].kind == TypeKind::Option)
        element = result_.typeTable[*expected].arguments[0];
      TypeId valueType = checkExpr(*value.arguments[0], element);
      TypeId result =
          expected.value_or(types_.intern(TypeKind::Option, {valueType}));
      if (result_.typeTable[result].kind != TypeKind::Option) {
        diagnostics_.error(expression.location,
                           "`Some` requires an expected `Option` type");
        return kError;
      }
      result_.references.insert_or_assign(value.callee->id,
                                          OptionConstructorReference{true});
      result_.expressionTypes.insert_or_assign(value.callee->id, kFunction);
      return result;
    }
    if (!expected || result_.typeTable[*expected].kind != TypeKind::Result) {
      checkExpr(*value.arguments[0]);
      diagnostics_.error(expression.location,
                         "`" + name.front() +
                             "` requires an expected `Result` type");
      return kError;
    }
    const bool ok = name.front() == "Ok";
    TypeId argument =
        checkExpr(*value.arguments[0],
                  result_.typeTable[*expected].arguments[ok ? 0 : 1]);
    TypeId wanted = result_.typeTable[*expected].arguments[ok ? 0 : 1];
    if (argument != kError && argument != kNever && argument != wanted)
      typeMismatch(value.arguments[0]->location, wanted, argument);
    result_.references.insert_or_assign(value.callee->id,
                                        ResultConstructorReference{ok});
    result_.expressionTypes.insert_or_assign(value.callee->id, kFunction);
    return *expected;
  }

  [[nodiscard]] std::optional<TypeId> nonSerializableType(TypeId id) const {
    const auto& type = result_.typeTable[id];
    switch (type.kind) {
    case TypeKind::Sender:
    case TypeKind::Receiver:
    case TypeKind::Task:
    case TypeKind::Listener:
    case TypeKind::Connection:
    case TypeKind::Range:
    case TypeKind::Function:
    case TypeKind::Never:
    case TypeKind::Error:
      return id;
    case TypeKind::Record: {
      if (!type.declaration)
        return id;
      const auto reference = std::get<RecordRef>(*type.declaration);
      const auto& declaration =
          at(at(modules_, reference.module).records, reference.record);
      for (TypeId field : result_.records.at(declaration.id).fields)
        if (auto rejected = nonSerializableType(field))
          return rejected;
      return std::nullopt;
    }
    case TypeKind::Enum: {
      if (!type.declaration)
        return id;
      const auto reference = std::get<EnumRef>(*type.declaration);
      const auto& declaration =
          at(at(modules_, reference.module).enums, reference.enumeration);
      for (const auto& variant : result_.enums.at(declaration.id).variants)
        for (TypeId payload : variant)
          if (auto rejected = nonSerializableType(payload))
            return rejected;
      return std::nullopt;
    }
    default:
      for (TypeId argument : type.arguments)
        if (auto rejected = nonSerializableType(argument))
          return rejected;
      return std::nullopt;
    }
  }

  std::optional<TypeId>
  resolveStandardErrorValue(const std::vector<std::string>& pathValue,
                            const Expr& expression, bool allowPayload) {
    if (pathValue.size() != 2)
      return std::nullopt;
    struct ErrorInfo {
      std::string_view name;
      TypeId type;
      StandardError error;
      std::vector<std::pair<std::string_view, std::vector<TypeId>>> variants;
    };
    const std::vector<ErrorInfo> errors = {
        {.name = "IndexError",
         .type = kIndexError,
         .error = StandardError::Index,
         .variants = {{"OutOfBounds", {}}}},
        {.name = "ChannelError",
         .type = kChannelError,
         .error = StandardError::Channel,
         .variants = {{"Closed", {}}}},
        {.name = "TaskError",
         .type = kTaskError,
         .error = StandardError::Task,
         .variants = {{"Panic", {kString}}}},
        {.name = "IoError",
         .type = kIoError,
         .error = StandardError::Io,
         .variants = {{"NotFound", {}},
                      {"PermissionDenied", {}},
                      {"InvalidUtf8", {}},
                      {"Other", {kString}}}},
        {.name = "TransportError",
         .type = kTransportError,
         .error = StandardError::Transport,
         .variants = {{"AddressInUse", {}},
                      {"ConnectionClosed", {}},
                      {"ConnectionReset", {}},
                      {"IncompatibleProtocol", {}},
                      {"InvalidMessage", {}},
                      {"MessageTooLarge", {}},
                      {"Other", {kString}}}},
        {.name = "ParseIntError",
         .type = kParseIntError,
         .error = StandardError::ParseInt,
         .variants = {{"Invalid", {}}, {"OutOfRange", {}}}},
    };
    auto error = std::ranges::find(errors, pathValue.front(), &ErrorInfo::name);
    if (error == errors.end())
      return std::nullopt;
    for (std::size_t index = 0; index < error->variants.size(); ++index) {
      const auto& [name, payloads] = error->variants[index];
      if (name != pathValue.back())
        continue;
      if (!allowPayload && !payloads.empty())
        return std::nullopt;
      result_.references.insert_or_assign(
          expression.id,
          StandardErrorConstructorReference{.error = error->error,
                                            .variant = VariantIndex{index}});
      return error->type;
    }
    diagnostics_.error(expression.location, "unknown standard error variant `" +
                                                pathValue.back() + "`");
    return kError;
  }

  std::optional<TypeId>
  resolveStandardErrorCall(const Expr& expression, const CallExpr& value,
                           const std::vector<std::string>& pathValue) {
    auto type = resolveStandardErrorValue(pathValue, *value.callee, true);
    if (!type || *type == kError)
      return type;
    const auto& reference = std::get<StandardErrorConstructorReference>(
        result_.references.at(value.callee->id));
    std::vector<TypeId> payloads;
    if ((reference.error == StandardError::Task &&
         reference.variant == VariantIndex{0}) ||
        (reference.error == StandardError::Io &&
         reference.variant == VariantIndex{3}) ||
        (reference.error == StandardError::Transport &&
         reference.variant == VariantIndex{6}))
      payloads = {kString};
    if (value.arguments.size() != payloads.size())
      diagnostics_.error(expression.location,
                         "standard error variant `" + pathValue.back() +
                             "` expects " + std::to_string(payloads.size()) +
                             " arguments");
    for (std::size_t index = 0; index < value.arguments.size(); ++index) {
      TypeId argument =
          checkExpr(*value.arguments[index],
                    index < payloads.size() ? std::optional(payloads[index])
                                            : std::nullopt);
      if (index < payloads.size() && argument != kError &&
          argument != payloads[index])
        typeMismatch(value.arguments[index]->location, payloads[index],
                     argument);
    }
    return type;
  }

  std::optional<TypeId>
  resolveVariantValue(const std::vector<std::string>& pathValue,
                      const Expr& expression, std::optional<TypeId>,
                      bool allowPayload) {
    if (pathValue.size() < 2 || pathValue.size() > 3)
      return std::nullopt;
    std::vector<std::string> typeNameParts(pathValue.begin(),
                                           pathValue.end() - 1);
    auto typeDecl = resolveTypeDecl(typeNameParts, expression.location, false);
    if (!typeDecl || !std::holds_alternative<EnumIndex>(typeDecl->declaration))
      return std::nullopt;
    const EnumIndex enumeration = std::get<EnumIndex>(typeDecl->declaration);
    const auto& declaration =
        at(at(modules_, typeDecl->module).enums, enumeration);
    const auto& info = result_.enums.at(declaration.id);
    for (std::size_t i = 0; i < declaration.variants.size(); ++i)
      if (declaration.variants.at(i).name == pathValue.back()) {
        if (!allowPayload && !info.variants.at(i).empty())
          return std::nullopt;
        result_.references.insert_or_assign(
            expression.id,
            EnumVariantReference{.enumeration =
                                     EnumRef{.module = typeDecl->module,
                                             .enumeration = enumeration},
                                 .variant = VariantIndex{i}});
        return info.type;
      }
    return std::nullopt;
  }

  std::optional<TypeId>
  resolveVariantCall(const Expr& expression, const CallExpr& value,
                     const std::vector<std::string>& pathValue,
                     std::optional<TypeId> expected) {
    auto type = resolveVariantValue(pathValue, *value.callee, expected, true);
    if (!type)
      return std::nullopt;
    const auto& reference =
        std::get<EnumVariantReference>(result_.references.at(value.callee->id));
    const auto& declaration =
        at(at(modules_, reference.enumeration.module).enums,
           reference.enumeration.enumeration);
    const auto& payloads =
        result_.enums.at(declaration.id).variants.at(reference.variant.value());
    if (value.arguments.size() != payloads.size())
      diagnostics_.error(expression.location,
                         "enum variant `" + pathValue.back() + "` expects " +
                             std::to_string(payloads.size()) + " arguments");
    for (size_t i = 0; i < value.arguments.size(); ++i) {
      TypeId argument = checkExpr(
          *value.arguments[i],
          i < payloads.size() ? std::optional(payloads[i]) : std::nullopt);
      if (i < payloads.size() && argument != kError && argument != kNever &&
          argument != payloads[i])
        typeMismatch(value.arguments[i]->location, payloads[i], argument);
    }
    return type;
  }

  TypeId checkMethod(const Expr& expression, const CallExpr& call,
                     const MemberExpr& callee, const std::string& name) {
    if (!call.typeArguments.empty())
      diagnostics_.error(expression.location,
                         "methods do not accept explicit type arguments");
    TypeId receiver = checkExpr(*callee.base);
    const auto& type = result_.typeTable[receiver];
    BuiltinMethod method = BuiltinMethod::StringLength;
    TypeId result = kError;
    std::vector<TypeId> parameters;
    if (type.kind == TypeKind::Int && name == "to_string") {
      method = BuiltinMethod::IntToString;
      result = kString;
    } else if (type.kind == TypeKind::String && name == "length") {
      method = BuiltinMethod::StringLength;
      result = kInt;
    } else if (type.kind == TypeKind::String && name == "split_once") {
      method = BuiltinMethod::StringSplitOnce;
      parameters = {kString};
      TypeId pair = types_.intern(TypeKind::Tuple, {kString, kString});
      result = types_.intern(TypeKind::Option, {pair});
    } else if (type.kind == TypeKind::List) {
      TypeId element = type.arguments[0];
      if (name == "length") {
        method = BuiltinMethod::ListLength;
        result = kInt;
      } else if (name == "is_empty") {
        method = BuiltinMethod::ListIsEmpty;
        result = kBool;
      } else if (name == "get") {
        method = BuiltinMethod::ListGet;
        parameters = {kInt};
        result = types_.intern(TypeKind::Option, {element});
      } else if (name == "push") {
        method = BuiltinMethod::ListPush;
        parameters = {element};
        result = receiver;
      } else if (name == "set") {
        method = BuiltinMethod::ListSet;
        parameters = {kInt, element};
        result = types_.intern(TypeKind::Result, {receiver, kIndexError});
      } else if (name == "remove_at") {
        method = BuiltinMethod::ListRemoveAt;
        parameters = {kInt};
        result = types_.intern(TypeKind::Result, {receiver, kIndexError});
      } else if (name == "take_last") {
        method = BuiltinMethod::ListTakeLast;
        parameters = {kInt};
        result = receiver;
      } else {
        return unknownMethod(expression, receiver, name);
      }
    } else if (type.kind == TypeKind::Map) {
      TypeId key = type.arguments[0], value = type.arguments[1];
      if (name == "length") {
        method = BuiltinMethod::MapLength;
        result = kInt;
      } else if (name == "is_empty") {
        method = BuiltinMethod::MapIsEmpty;
        result = kBool;
      } else if (name == "contains") {
        method = BuiltinMethod::MapContains;
        parameters = {key};
        result = kBool;
      } else if (name == "get") {
        method = BuiltinMethod::MapGet;
        parameters = {key};
        result = types_.intern(TypeKind::Option, {value});
      } else if (name == "insert") {
        method = BuiltinMethod::MapInsert;
        parameters = {key, value};
        result = receiver;
      } else if (name == "remove") {
        method = BuiltinMethod::MapRemove;
        parameters = {key};
        result = receiver;
      } else if (name == "entries") {
        method = BuiltinMethod::MapEntries;
        TypeId tuple = types_.intern(TypeKind::Tuple, {key, value});
        result = types_.intern(TypeKind::List, {tuple});
      } else {
        return unknownMethod(expression, receiver, name);
      }
    } else if (type.kind == TypeKind::Sender) {
      if (name == "send") {
        method = BuiltinMethod::SenderSend;
        parameters = {type.arguments.front()};
        result = types_.intern(TypeKind::Result, {kUnit, kChannelError});
      } else if (name == "close") {
        method = BuiltinMethod::SenderClose;
        result = kUnit;
      } else {
        return unknownMethod(expression, receiver, name);
      }
    } else if (type.kind == TypeKind::Receiver) {
      if (name != "receive")
        return unknownMethod(expression, receiver, name);
      method = BuiltinMethod::ReceiverReceive;
      result = types_.intern(TypeKind::Option, {type.arguments.front()});
    } else if (type.kind == TypeKind::Task) {
      if (name != "join")
        return unknownMethod(expression, receiver, name);
      method = BuiltinMethod::TaskJoin;
      result =
          types_.intern(TypeKind::Result, {type.arguments.front(), kTaskError});
    } else if (type.kind == TypeKind::Listener) {
      if (name == "accept") {
        method = BuiltinMethod::ListenerAccept;
        TypeId connection = types_.intern(TypeKind::Connection, type.arguments);
        result = types_.intern(TypeKind::Result, {connection, kTransportError});
      } else if (name == "local_address") {
        method = BuiltinMethod::ListenerLocalAddress;
        result = types_.intern(TypeKind::Result, {kString, kTransportError});
      } else if (name == "close") {
        method = BuiltinMethod::ListenerClose;
        result = kUnit;
      } else {
        return unknownMethod(expression, receiver, name);
      }
    } else if (type.kind == TypeKind::Connection) {
      if (name == "send") {
        method = BuiltinMethod::ConnectionSend;
        parameters = {type.arguments.at(1)};
        result = types_.intern(TypeKind::Result, {kUnit, kTransportError});
      } else if (name == "receive") {
        method = BuiltinMethod::ConnectionReceive;
        TypeId option = types_.intern(TypeKind::Option, {type.arguments.at(0)});
        result = types_.intern(TypeKind::Result, {option, kTransportError});
      } else if (name == "close") {
        method = BuiltinMethod::ConnectionClose;
        result = kUnit;
      } else {
        return unknownMethod(expression, receiver, name);
      }
    } else {
      return unknownMethod(expression, receiver, name);
    }
    if (call.arguments.size() != parameters.size())
      diagnostics_.error(expression.location,
                         "method `" + name + "` expects " +
                             std::to_string(parameters.size()) + " arguments");
    for (size_t i = 0; i < call.arguments.size(); ++i) {
      TypeId argument = checkExpr(
          *call.arguments[i],
          i < parameters.size() ? std::optional(parameters[i]) : std::nullopt);
      if (i < parameters.size() && argument != kError && argument != kNever &&
          argument != parameters[i])
        typeMismatch(call.arguments[i]->location, parameters[i], argument);
    }
    result_.references.insert_or_assign(call.callee->id,
                                        BuiltinMethodReference{method});
    result_.expressionTypes.insert_or_assign(call.callee->id, kFunction);
    return result;
  }

  TypeId unknownMethod(const Expr& expression, TypeId receiver,
                       const std::string& name) {
    diagnostics_.error(expression.location, "unknown method `" + name +
                                                "` for `" + typeName(receiver) +
                                                "`");
    return kError;
  }

  TypeId checkRecord(const Expr& expression, const RecordExpr& value) {
    auto declaration =
        resolveTypeDecl(value.typeName, expression.location, true);
    if (!declaration ||
        !std::holds_alternative<RecordIndex>(declaration->declaration)) {
      if (declaration)
        diagnostics_.error(expression.location,
                           "record construction requires a record type");
      return kError;
    }
    const RecordRef recordReference{
        .module = declaration->module,
        .record = std::get<RecordIndex>(declaration->declaration)};
    const auto& record = at(at(modules_, recordReference.module).records,
                            recordReference.record);
    const auto& info = result_.records.at(record.id);
    std::vector<bool> seen(record.fields.size());
    for (const auto& field : value.fields) {
      auto found =
          std::ranges::find(record.fields, field.name, &RecordFieldDecl::name);
      if (found == record.fields.end()) {
        diagnostics_.error(field.location,
                           "unknown record field `" + field.name + "`");
        checkExpr(*field.value);
        continue;
      }
      size_t index = static_cast<size_t>(found - record.fields.begin());
      if (seen[index])
        diagnostics_.error(field.location,
                           "duplicate record field `" + field.name + "`");
      seen[index] = true;
      TypeId fieldType = checkExpr(*field.value, info.fields[index]);
      if (fieldType != kError && fieldType != kNever &&
          fieldType != info.fields[index])
        typeMismatch(field.value->location, info.fields[index], fieldType);
    }
    for (size_t i = 0; i < seen.size(); ++i)
      if (!seen[i])
        diagnostics_.error(expression.location, "missing record field `" +
                                                    record.fields[i].name +
                                                    "`");
    result_.references.insert_or_assign(
        expression.id, RecordConstructorReference{recordReference});
    return info.type;
  }

  TypeId checkRecordUpdate(const Expr& expression,
                           const RecordUpdateExpr& value) {
    TypeId base = checkExpr(*value.base);
    if (result_.typeTable[base].kind != TypeKind::Record) {
      diagnostics_.error(expression.location,
                         "record update requires a record value");
      return kError;
    }
    const auto& semantic = result_.typeTable[base];
    if (!semantic.declaration)
      return kError;
    const RecordRef recordReference =
        std::get<RecordRef>(semantic.declaration.value());
    const auto& record = at(at(modules_, recordReference.module).records,
                            recordReference.record);
    const auto& info = result_.records.at(record.id);
    std::unordered_set<std::string> seen;
    for (const auto& field : value.fields) {
      auto found =
          std::ranges::find(record.fields, field.name, &RecordFieldDecl::name);
      if (found == record.fields.end()) {
        diagnostics_.error(field.location,
                           "unknown record field `" + field.name + "`");
        checkExpr(*field.value);
        continue;
      }
      if (!seen.insert(field.name).second)
        diagnostics_.error(field.location,
                           "duplicate record field `" + field.name + "`");
      size_t index = static_cast<size_t>(found - record.fields.begin());
      TypeId fieldType = checkExpr(*field.value, info.fields[index]);
      if (fieldType != kError && fieldType != kNever &&
          fieldType != info.fields[index])
        typeMismatch(field.value->location, info.fields[index], fieldType);
    }
    return base;
  }

  void bindPattern(const Pattern& pattern, TypeId type,
                   std::unordered_set<std::string>& names) {
    std::visit(
        Overloaded{
            [&](const WildcardPattern&) {},
            [&](const BindingPattern& binding) {
              if (!names.insert(binding.name).second) {
                diagnostics_.error(pattern.location,
                                   "duplicate pattern binding `" +
                                       binding.name + "`");
                return;
              }
              const LocalId id = nextLocal_;
              nextLocal_ = LocalId{nextLocal_.value() + 1};
              scopes_.back().insert_or_assign(
                  binding.name,
                  Local{.id = id, .type = type, .mutableBinding = false});
              result_.patternBindings.insert_or_assign(pattern.id, id);
            },
            [&](const LiteralPattern& literal) {
              TypeId literalType =
                  std::visit(Overloaded{[](bool) -> TypeId { return kBool; },
                                        [](int64_t) -> TypeId { return kInt; },
                                        [](const std::string&) -> TypeId {
                                          return kString;
                                        }},
                             literal.value);
              if (literalType != type)
                typeMismatch(pattern.location, type, literalType);
            },
            [&](const ConstructorPattern& constructor) {
              bindConstructorPattern(pattern, constructor, type, names);
            }},
        pattern.value);
  }

  void bindConstructorPattern(const Pattern& pattern,
                              const ConstructorPattern& constructor,
                              TypeId type,
                              std::unordered_set<std::string>& names) {
    const auto& semantic = result_.typeTable[type];
    VariantIndex index{0};
    std::vector<TypeId> payloads;
    ResolvedReference reference = IndexErrorConstructorReference{};
    if (semantic.kind == TypeKind::Option) {
      if (constructor.name.size() != 1 ||
          (constructor.name[0] != "Some" && constructor.name[0] != "None")) {
        diagnostics_.error(pattern.location, "invalid `Option` pattern");
        return;
      }
      const bool some = constructor.name[0] == "Some";
      index = VariantIndex{some ? 1U : 0U};
      reference = OptionConstructorReference{some};
      if (some)
        payloads = {semantic.arguments[0]};
    } else if (semantic.kind == TypeKind::Result) {
      if (constructor.name.size() != 1 ||
          (constructor.name[0] != "Ok" && constructor.name[0] != "Err")) {
        diagnostics_.error(pattern.location, "invalid `Result` pattern");
        return;
      }
      const bool ok = constructor.name[0] == "Ok";
      index = VariantIndex{ok ? 0U : 1U};
      reference = ResultConstructorReference{ok};
      payloads = {semantic.arguments.at(index.value())};
    } else if (semantic.kind == TypeKind::IndexError ||
               semantic.kind == TypeKind::ChannelError ||
               semantic.kind == TypeKind::TaskError ||
               semantic.kind == TypeKind::IoError ||
               semantic.kind == TypeKind::TransportError ||
               semantic.kind == TypeKind::ParseIntError) {
      struct StandardPatternInfo {
        TypeKind kind;
        std::string_view name;
        StandardError error;
        std::vector<std::pair<std::string_view, std::vector<TypeId>>> variants;
      };
      const std::vector<StandardPatternInfo> errors = {
          {.kind = TypeKind::IndexError,
           .name = "IndexError",
           .error = StandardError::Index,
           .variants = {{"OutOfBounds", {}}}},
          {.kind = TypeKind::ChannelError,
           .name = "ChannelError",
           .error = StandardError::Channel,
           .variants = {{"Closed", {}}}},
          {.kind = TypeKind::TaskError,
           .name = "TaskError",
           .error = StandardError::Task,
           .variants = {{"Panic", {kString}}}},
          {.kind = TypeKind::IoError,
           .name = "IoError",
           .error = StandardError::Io,
           .variants = {{"NotFound", {}},
                        {"PermissionDenied", {}},
                        {"InvalidUtf8", {}},
                        {"Other", {kString}}}},
          {.kind = TypeKind::TransportError,
           .name = "TransportError",
           .error = StandardError::Transport,
           .variants = {{"AddressInUse", {}},
                        {"ConnectionClosed", {}},
                        {"ConnectionReset", {}},
                        {"IncompatibleProtocol", {}},
                        {"InvalidMessage", {}},
                        {"MessageTooLarge", {}},
                        {"Other", {kString}}}},
          {.kind = TypeKind::ParseIntError,
           .name = "ParseIntError",
           .error = StandardError::ParseInt,
           .variants = {{"Invalid", {}}, {"OutOfRange", {}}}},
      };
      const auto error =
          std::ranges::find(errors, semantic.kind, &StandardPatternInfo::kind);
      if (error == errors.end() || constructor.name.size() != 2 ||
          constructor.name[0] != error->name) {
        diagnostics_.error(pattern.location, "invalid standard error pattern");
        return;
      }
      const auto variant = std::ranges::find(
          error->variants, constructor.name[1],
          &std::pair<std::string_view, std::vector<TypeId>>::first);
      if (variant == error->variants.end()) {
        diagnostics_.error(pattern.location,
                           "unknown standard error variant `" +
                               constructor.name[1] + "`");
        return;
      }
      index = VariantIndex{
          static_cast<std::size_t>(variant - error->variants.begin())};
      payloads = variant->second;
      reference = StandardErrorConstructorReference{.error = error->error,
                                                    .variant = index};
    } else if (semantic.kind == TypeKind::Enum) {
      if (!semantic.declaration)
        return;
      const EnumRef enumReference =
          std::get<EnumRef>(semantic.declaration.value());
      const auto& declaration = at(at(modules_, enumReference.module).enums,
                                   enumReference.enumeration);
      if (constructor.name.empty() || constructor.name.back().empty())
        return;
      auto found =
          std::ranges::find(declaration.variants, constructor.name.back(),
                            &EnumVariantDecl::name);
      if (found == declaration.variants.end()) {
        diagnostics_.error(pattern.location, "unknown enum variant `" +
                                                 constructor.name.back() + "`");
        return;
      }
      index = VariantIndex{
          static_cast<std::size_t>(found - declaration.variants.begin())};
      payloads = result_.enums.at(declaration.id).variants.at(index.value());
      reference =
          EnumVariantReference{.enumeration = enumReference, .variant = index};
    } else {
      diagnostics_.error(pattern.location,
                         "constructor pattern requires an enum-like value");
      return;
    }
    if (constructor.arguments.size() != payloads.size())
      diagnostics_.error(pattern.location,
                         "pattern payload arity does not match variant");
    result_.patterns.insert_or_assign(pattern.id, reference);
    for (size_t i = 0; i < constructor.arguments.size() && i < payloads.size();
         ++i)
      bindPattern(*constructor.arguments[i], payloads[i], names);
  }

  [[nodiscard]] bool irrefutable(const Pattern& pattern) const {
    return std::holds_alternative<WildcardPattern>(pattern.value) ||
           std::holds_alternative<BindingPattern>(pattern.value);
  }

  struct MatchCase {
    std::string name;
    std::vector<MatchCase> payloads;
  };

  [[nodiscard]] std::vector<MatchCase> requiredCases(TypeId type) const {
    const auto& semantic = result_.typeTable[type];
    if (semantic.kind == TypeKind::Bool)
      return {{.name = "false", .payloads = {}},
              {.name = "true", .payloads = {}}};
    if (semantic.kind == TypeKind::Int || semantic.kind == TypeKind::String)
      return {{.name = "*", .payloads = {}}};
    if (semantic.kind == TypeKind::IndexError)
      return {{.name = "OutOfBounds", .payloads = {}}};
    if (semantic.kind == TypeKind::ChannelError)
      return {{.name = "Closed", .payloads = {}}};
    if (semantic.kind == TypeKind::TaskError)
      return {{.name = "Panic", .payloads = {{.name = "*", .payloads = {}}}}};
    if (semantic.kind == TypeKind::IoError)
      return {{.name = "NotFound", .payloads = {}},
              {.name = "PermissionDenied", .payloads = {}},
              {.name = "InvalidUtf8", .payloads = {}},
              {.name = "Other", .payloads = {{.name = "*", .payloads = {}}}}};
    if (semantic.kind == TypeKind::TransportError)
      return {{.name = "AddressInUse", .payloads = {}},
              {.name = "ConnectionClosed", .payloads = {}},
              {.name = "ConnectionReset", .payloads = {}},
              {.name = "IncompatibleProtocol", .payloads = {}},
              {.name = "InvalidMessage", .payloads = {}},
              {.name = "MessageTooLarge", .payloads = {}},
              {.name = "Other", .payloads = {{.name = "*", .payloads = {}}}}};
    if (semantic.kind == TypeKind::ParseIntError)
      return {{.name = "Invalid", .payloads = {}},
              {.name = "OutOfRange", .payloads = {}}};
    std::vector<std::pair<std::string, std::vector<TypeId>>> constructors;
    if (semantic.kind == TypeKind::Option)
      constructors = {{"None", {}}, {"Some", {semantic.arguments[0]}}};
    else if (semantic.kind == TypeKind::Result)
      constructors = {{"Ok", {semantic.arguments[0]}},
                      {"Err", {semantic.arguments[1]}}};
    else if (semantic.kind == TypeKind::Enum) {
      if (!semantic.declaration)
        return {};
      const EnumRef enumReference =
          std::get<EnumRef>(semantic.declaration.value());
      const auto& declaration = at(at(modules_, enumReference.module).enums,
                                   enumReference.enumeration);
      const auto& variants = result_.enums.at(declaration.id).variants;
      for (size_t index = 0; index < declaration.variants.size(); ++index)
        constructors.emplace_back(declaration.variants[index].name,
                                  variants[index]);
    } else {
      return {};
    }
    std::vector<MatchCase> result;
    for (const auto& [name, payloadTypes] : constructors) {
      std::vector<MatchCase> partial{{.name = name, .payloads = {}}};
      for (TypeId payloadType : payloadTypes) {
        auto payloadCases = requiredCases(payloadType);
        if (payloadCases.empty())
          payloadCases.push_back({.name = "*", .payloads = {}});
        std::vector<MatchCase> expanded;
        for (const auto& item : partial)
          for (const auto& payload : payloadCases) {
            auto next = item;
            next.payloads.push_back(payload);
            expanded.push_back(std::move(next));
          }
        partial = std::move(expanded);
      }
      result.insert(result.end(), partial.begin(), partial.end());
    }
    return result;
  }

  [[nodiscard]] bool patternCovers(const Pattern& pattern,
                                   const MatchCase& item) const {
    if (irrefutable(pattern))
      return true;
    if (const auto* literal = std::get_if<LiteralPattern>(&pattern.value)) {
      const std::string spelling = std::visit(
          Overloaded{
              [](bool value) { return std::string(value ? "true" : "false"); },
              [](int64_t value) { return std::to_string(value); },
              [](const std::string& value) { return value; }},
          literal->value);
      return item.name != "*" && spelling == item.name;
    }
    const auto& constructor = std::get<ConstructorPattern>(pattern.value);
    if (constructor.name.back() != item.name ||
        constructor.arguments.size() != item.payloads.size())
      return false;
    for (size_t index = 0; index < constructor.arguments.size(); ++index)
      if (!patternCovers(*constructor.arguments[index], item.payloads[index]))
        return false;
    return true;
  }

  [[nodiscard]] bool patternCouldMatch(const Pattern& pattern,
                                       const MatchCase& item) const {
    if (irrefutable(pattern))
      return true;
    if (std::holds_alternative<LiteralPattern>(pattern.value))
      return item.name == "*" || patternCovers(pattern, item);
    const auto& constructor = std::get<ConstructorPattern>(pattern.value);
    if (constructor.name.back() != item.name ||
        constructor.arguments.size() != item.payloads.size())
      return false;
    for (size_t index = 0; index < constructor.arguments.size(); ++index)
      if (!patternCouldMatch(*constructor.arguments[index],
                             item.payloads[index]))
        return false;
    return true;
  }

  [[nodiscard]] std::string patternName(const Pattern& pattern) const {
    if (irrefutable(pattern))
      return "_";
    if (const auto* literal = std::get_if<LiteralPattern>(&pattern.value))
      return std::visit(
          Overloaded{
              [](bool value) { return std::string(value ? "true" : "false"); },
              [](int64_t value) { return std::to_string(value); },
              [](const std::string& value) { return '"' + value + '"'; }},
          literal->value);
    const auto& constructor = std::get<ConstructorPattern>(pattern.value);
    std::string result = constructor.name.back();
    if (constructor.arguments.empty())
      return result;
    result += '(';
    for (size_t index = 0; index < constructor.arguments.size(); ++index) {
      if (index)
        result += ',';
      result += patternName(*constructor.arguments[index]);
    }
    return result + ')';
  }

  [[nodiscard]] std::string caseName(const MatchCase& item) const {
    if (item.payloads.empty())
      return item.name;
    std::string result = item.name + "(";
    for (size_t index = 0; index < item.payloads.size(); ++index) {
      if (index)
        result += ", ";
      result += caseName(item.payloads[index]);
    }
    return result + ")";
  }

  [[nodiscard]] std::optional<std::string> topCase(const Pattern& pattern,
                                                   TypeId type) const {
    if (const auto* literal = std::get_if<LiteralPattern>(&pattern.value))
      return std::visit(
          Overloaded{
              [](bool value) { return std::string(value ? "true" : "false"); },
              [](int64_t value) { return std::to_string(value); },
              [](const std::string& value) { return "\"" + value + "\""; }},
          literal->value);
    const auto* constructor = std::get_if<ConstructorPattern>(&pattern.value);
    if (!constructor)
      return std::nullopt;
    if (result_.typeTable[type].kind == TypeKind::Enum)
      return constructor->name.back();
    return constructor->name.front();
  }

  TypeId checkMatch(const Expr& expression, const MatchExpr& match,
                    std::optional<TypeId> expected) {
    TypeId scrutinee = checkExpr(*match.value);
    TypeId result = kError;
    std::vector<MatchCase> uncovered = requiredCases(scrutinee);
    const std::vector<MatchCase> possibleCases = uncovered;
    const bool supported = !possibleCases.empty();
    std::set<std::string> seenPatterns;
    for (const auto& arm : match.arms) {
      const bool canMatch =
          std::ranges::any_of(possibleCases, [&](const MatchCase& item) {
            return patternCouldMatch(*arm.pattern, item);
          });
      const bool coversUncovered =
          std::ranges::any_of(uncovered, [&](const MatchCase& item) {
            return patternCouldMatch(*arm.pattern, item);
          });
      const bool unique = seenPatterns.insert(patternName(*arm.pattern)).second;
      if (canMatch && (!coversUncovered || !unique))
        diagnostics_.error(arm.location, "unreachable match arm");
      if (canMatch)
        std::erase_if(uncovered, [&](const MatchCase& item) {
          return patternCovers(*arm.pattern, item);
        });
      scopes_.emplace_back();
      std::unordered_set<std::string> names;
      bindPattern(*arm.pattern, scrutinee, names);
      TypeId armType = checkExpr(*arm.value, expected);
      scopes_.pop_back();
      result = result == kError
                   ? armType
                   : mergeBranches(expression.location, result, armType);
    }
    if (!supported) {
      diagnostics_.error(expression.location,
                         "match requires an enum-like or literal value");
      return result;
    }
    if (uncovered.empty())
      return result;
    std::string message = "non-exhaustive match, missing ";
    for (size_t index = 0; index < uncovered.size(); ++index) {
      if (index)
        message += ", ";
      message += '`' + caseName(uncovered[index]) + '`';
    }
    diagnostics_.error(expression.location, message);
    return result;
  }

  const std::vector<Module>& modules_;
  DiagnosticEngine& diagnostics_;
  ModuleGraph graph_;
  SemanticResult result_;
  ModuleId currentModule_{0};
  const FunctionDecl* currentFunction_ = nullptr;
  TypeId currentReturn_ = kUnit;
  std::vector<std::unordered_map<std::string, Local>> scopes_;
  LocalId nextLocal_{0};
  std::size_t loopDepth_ = 0;
  TypeContext types_;
};

} // namespace

std::optional<SemanticResult> analyze(const std::vector<Module>& modules,
                                      DiagnosticEngine& diagnostics,
                                      bool requireEntryPoint) {
  return Analyzer(modules, diagnostics).run(requireEntryPoint);
}

} // namespace dola
