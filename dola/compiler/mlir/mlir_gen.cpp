#include "dola/mlir_gen.h"

#include "dola/ast_analysis.h"
#include "dola/dola_dialect.h"
#include "dola/overloaded.h"
#include "dola/runtime_operations.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/MLIRContext.h"

#include <cassert>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>

namespace dola {

std::string mangleFunction(const Module& module, const FunctionDecl& function) {
  std::string result = "__dola_";
  for (const auto& component : module.name)
    result += component + "_";
  return result + function.name;
}

namespace {

struct FunctionReturnStatus {
  mlir::Value value;
};

struct FunctionReturnValue {
  mlir::Value value;
};

struct LocalValue {
  mlir::Value value;
  TypeId type;
  bool address;
  bool owned;
};

class LocalEnvironment {
public:
  void reset() {
    values_.clear();
    scopes_.clear();
    scopes_.emplace_back();
  }

  void enterScope() { scopes_.emplace_back(); }
  void leaveScope() { scopes_.pop_back(); }

  void bind(LocalId id, LocalValue value) {
    values_.insert_or_assign(id, value);
    scopes_.back().push_back(id);
  }

  void bindParameter(LocalId id, LocalValue value) {
    values_.insert_or_assign(id, value);
  }

  [[nodiscard]] LocalValue& get(LocalId id) { return values_.at(id); }
  [[nodiscard]] const LocalValue& get(LocalId id) const {
    return values_.at(id);
  }

  [[nodiscard]] std::size_t scopeDepth() const { return scopes_.size(); }

  template <typename Release>
  void cleanupCurrent(const Release& release) const {
    for (LocalId id : scopes_.back())
      release(id);
  }

  template <typename Release> void cleanupAll(const Release& release) const {
    for (const auto& scope : std::views::reverse(scopes_))
      for (LocalId id : scope)
        release(id);
  }

  template <typename Release>
  void cleanupExited(std::size_t retainedDepth, const Release& release) const {
    for (std::size_t index = scopes_.size(); index > retainedDepth; --index)
      for (LocalId id : scopes_.at(index - 1))
        release(id);
  }

private:
  std::unordered_map<LocalId, LocalValue> values_;
  std::vector<std::vector<LocalId>> scopes_;
};

struct LoopTargets {
  mlir::Block* breakTarget;
  mlir::Block* continueTarget;
  mlir::Value continueValue;
  size_t scopeDepth;
};

struct FunctionRefHash {
  std::size_t operator()(FunctionRef reference) const noexcept {
    const std::size_t left = std::hash<ModuleId>{}(reference.module);
    const std::size_t right = std::hash<FunctionIndex>{}(reference.function);
    return left ^ (right + 0x9e3779b9U + (left << 6U) + (left >> 2U));
  }
};

std::size_t constructorTag(const ResolvedReference& reference) {
  return std::visit(
      Overloaded{[](const OptionConstructorReference& value) -> std::size_t {
                   return value.some ? 1 : 0;
                 },
                 [](const ResultConstructorReference& value) -> std::size_t {
                   return value.ok ? 0 : 1;
                 },
                 [](const EnumVariantReference& value) -> std::size_t {
                   return value.variant.value();
                 },
                 [](const IndexErrorConstructorReference&) -> std::size_t {
                   return 0;
                 },
                 [](const StandardErrorConstructorReference& value)
                     -> std::size_t { return value.variant.value(); },
                 [](const auto&) -> std::size_t { return 0; }},
      reference);
}

class Generator {
public:
  Generator(mlir::MLIRContext& context, const std::vector<Module>& modules,
            const SemanticResult& semantics)
      : context_(context), modules_(modules), semantics_(semantics),
        builder_(&context) {}

  mlir::OwningOpRef<mlir::ModuleOp> run() {
    auto module = mlir::ModuleOp::create(builder_.getUnknownLoc());
    module_ = module;
    for (std::size_t moduleIndex = 0; moduleIndex < modules_.size();
         ++moduleIndex) {
      const ModuleId moduleId{moduleIndex};
      for (std::size_t functionIndex = 0;
           functionIndex < at(modules_, moduleId).functions.size();
           ++functionIndex)
        declareFunction(FunctionRef{.module = moduleId,
                                    .function = FunctionIndex{functionIndex}});
    }
    for (std::size_t moduleIndex = 0; moduleIndex < modules_.size();
         ++moduleIndex) {
      const ModuleId moduleId{moduleIndex};
      for (std::size_t functionIndex = 0;
           functionIndex < at(modules_, moduleId).functions.size();
           ++functionIndex)
        defineFunction(FunctionRef{.module = moduleId,
                                   .function = FunctionIndex{functionIndex}});
    }
    if (typeIdentityCollision_)
      return {};
    return mlir::OwningOpRef<mlir::ModuleOp>(module);
  }

private:
  mlir::Location loc(const SourceLocation& source) {
    return mlir::FileLineColLoc::get(&context_, source.path.string(),
                                     source.line, source.column);
  }

  mlir::Type type(TypeId id) {
    const auto& value = semantics_.typeTable[id];
    switch (value.kind) {
    case TypeKind::Unit:
      return ir::UnitType::get(&context_);
    case TypeKind::Never:
      return {};
    case TypeKind::Bool:
      return builder_.getI1Type();
    case TypeKind::Int:
      return builder_.getI64Type();
    case TypeKind::Float:
      return builder_.getF64Type();
    case TypeKind::String:
      return ir::StringType::get(&context_);
    case TypeKind::Record: {
      if (!value.declaration)
        return {};
      const RecordRef declaration =
          std::get<RecordRef>(value.declaration.value());
      return ir::RecordType::get(
          &context_,
          qualifiedTypeName(
              declaration.module,
              at(at(modules_, declaration.module).records, declaration.record)
                  .name));
    }
    case TypeKind::Enum: {
      if (!value.declaration)
        return {};
      const EnumRef declaration = std::get<EnumRef>(value.declaration.value());
      return ir::EnumType::get(
          &context_,
          qualifiedTypeName(declaration.module,
                            at(at(modules_, declaration.module).enums,
                               declaration.enumeration)
                                .name));
    }
    case TypeKind::Tuple: {
      llvm::SmallVector<mlir::Type> elements;
      for (TypeId element : value.arguments)
        elements.push_back(type(element));
      return ir::TupleType::get(&context_, elements);
    }
    case TypeKind::Option:
      return ir::OptionType::get(&context_, type(value.arguments[0]));
    case TypeKind::Result:
      return ir::ResultType::get(&context_, type(value.arguments[0]),
                                 type(value.arguments[1]));
    case TypeKind::List:
      return ir::ListType::get(&context_, type(value.arguments[0]));
    case TypeKind::Map:
      return ir::MapType::get(&context_, type(value.arguments[0]),
                              type(value.arguments[1]));
    case TypeKind::Sender:
      return ir::SenderType::get(&context_, type(value.arguments[0]));
    case TypeKind::Receiver:
      return ir::ReceiverType::get(&context_, type(value.arguments[0]));
    case TypeKind::Task:
      return ir::TaskType::get(&context_, type(value.arguments[0]));
    case TypeKind::Listener:
      return ir::ListenerType::get(&context_, type(value.arguments[0]),
                                   type(value.arguments[1]));
    case TypeKind::Connection:
      return ir::ConnectionType::get(&context_, type(value.arguments[0]),
                                     type(value.arguments[1]));
    case TypeKind::IndexError:
      return ir::EnumType::get(&context_, "std.IndexError");
    case TypeKind::ChannelError:
      return ir::EnumType::get(&context_, "std.ChannelError");
    case TypeKind::TaskError:
      return ir::EnumType::get(&context_, "std.TaskError");
    case TypeKind::IoError:
      return ir::EnumType::get(&context_, "std.IoError");
    case TypeKind::TransportError:
      return ir::EnumType::get(&context_, "std.TransportError");
    case TypeKind::ParseIntError:
      return ir::EnumType::get(&context_, "std.ParseIntError");
    case TypeKind::Range:
    case TypeKind::Function:
    case TypeKind::Error:
      return {};
    }
    return {};
  }

  [[nodiscard]] std::string qualifiedTypeName(ModuleId module,
                                              const std::string& name) const {
    return qualifiedName(at(modules_, module).name) + "." + name;
  }

  [[nodiscard]] std::string canonicalType(TypeId id) const {
    const auto& semantic = semantics_.typeTable[id];
    auto arguments = [&](llvm::StringRef name) {
      std::string result = name.str() + '[';
      for (TypeId argument : semantic.arguments) {
        const std::string nested = canonicalType(argument);
        result += std::to_string(nested.size()) + ':' + nested;
      }
      return result + ']';
    };
    if (semantic.kind == TypeKind::Record) {
      if (!semantic.declaration.has_value())
        return "invalid-record";
      const RecordRef reference =
          std::get<RecordRef>(semantic.declaration.value());
      const auto& declaration =
          at(at(modules_, reference.module).records, reference.record);
      std::string result = "record:" +
                           qualifiedTypeName(reference.module, declaration.name) +
                           '{';
      const auto& fields = semantics_.records.at(declaration.id).fields;
      for (std::size_t index = 0; index < fields.size(); ++index) {
        const std::string nested = canonicalType(fields.at(index));
        const std::string& fieldName = declaration.fields.at(index).name;
        result += std::to_string(fieldName.size());
        result += ':';
        result += fieldName;
        result += std::to_string(nested.size());
        result += ':';
        result += nested;
      }
      return result + '}';
    }
    if (semantic.kind == TypeKind::Enum) {
      if (!semantic.declaration.has_value())
        return "invalid-enum";
      const EnumRef reference =
          std::get<EnumRef>(semantic.declaration.value());
      const auto& declaration =
          at(at(modules_, reference.module).enums, reference.enumeration);
      std::string result = "enum:" +
                           qualifiedTypeName(reference.module, declaration.name) +
                           '{';
      const auto& variants = semantics_.enums.at(declaration.id).variants;
      for (std::size_t index = 0; index < variants.size(); ++index) {
        const std::string& variantName = declaration.variants.at(index).name;
        result += std::to_string(variantName.size()) + ':' + variantName + '(';
        for (TypeId payload : variants.at(index)) {
          const std::string nested = canonicalType(payload);
          result += std::to_string(nested.size()) + ':' + nested;
        }
        result += ')';
      }
      return result + '}';
    }
    switch (semantic.kind) {
    case TypeKind::Tuple:
      return arguments("tuple");
    case TypeKind::Option:
      return arguments("option");
    case TypeKind::Result:
      return arguments("result");
    case TypeKind::List:
      return arguments("list");
    case TypeKind::Map:
      return arguments("map");
    default:
      return semantics_.typeTable.name(id, modules_);
    }
  }

  [[nodiscard]] RuntimeTypeId typeId(TypeId id) {
    const std::string spelling = canonicalType(id);
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : spelling) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
    const auto [existing, inserted] =
        runtimeTypeSpellings_.try_emplace(hash, spelling);
    if (!inserted && existing->second != spelling) {
      module_.emitError("runtime type identity collision between `")
          << existing->second << "` and `" << spelling << '`';
      typeIdentityCollision_ = true;
    }
    return RuntimeTypeId{hash};
  }

  mlir::FunctionType functionType(const FunctionDecl& function) {
    const auto& info = semantics_.functions.at(function.id);
    llvm::SmallVector<mlir::Type> inputs, results;
    inputs.push_back(ir::ContextType::get(&context_));
    for (TypeId parameter : info.parameters)
      inputs.push_back(type(parameter));
    results.push_back(builder_.getI32Type());
    if (info.result != TypeId{0})
      results.push_back(type(info.result));
    return builder_.getFunctionType(inputs, results);
  }

  void declareFunction(FunctionRef reference) {
    const auto& declaration =
        at(at(modules_, reference.module).functions, reference.function);
    builder_.setInsertionPointToEnd(module_.getBody());
    auto operation = builder_.create<mlir::func::FuncOp>(
        loc(declaration.location),
        mangleFunction(at(modules_, reference.module), declaration),
        functionType(declaration));
    functions_.insert_or_assign(reference, operation);
  }

  void defineFunction(FunctionRef reference) {
    currentFunction_ = reference;
    environment_.reset();
    loopTargets_.clear();
    auto operation = functions_.at(reference);
    auto* entry = operation.addEntryBlock();
    builder_.setInsertionPointToStart(entry);
    const auto& declaration =
        at(at(modules_, reference.module).functions, reference.function);
    const auto& info = semantics_.functions.at(declaration.id);
    currentContext_ = entry->getArgument(0);
    currentResult_ = info.result;
    for (std::size_t i = 0; i < declaration.parameters.size(); ++i)
      environment_.bindParameter(LocalId{i},
                                 LocalValue{.value = entry->getArgument(i + 1),
                                            .type = info.parameters.at(i),
                                            .address = false,
                                            .owned = false});
    auto tail = emitBlock(*declaration.body, false);
    if (!terminated()) {
      if (info.result == TypeId{0}) {
        cleanupAll();
        emitFunctionReturn(loc(declaration.body->location),
                           FunctionReturnStatus{status(0)}, {});
      } else if (tail) {
        cleanupAll();
        emitFunctionReturn(loc(declaration.body->location),
                           FunctionReturnStatus{status(0)},
                           FunctionReturnValue{*tail});
      }
    }
  }

  mlir::Value status(std::int32_t value) {
    return builder_.create<mlir::arith::ConstantIntOp>(builder_.getUnknownLoc(),
                                                        value, 32);
  }

  mlir::Value poison(mlir::Location location) {
    if (currentResult_ == TypeId{0})
      return {};
    return builder_.create<ir::PoisonOp>(location, type(currentResult_));
  }

  void emitFunctionReturn(mlir::Location location,
                          FunctionReturnStatus returnStatus,
                          FunctionReturnValue value) {
    llvm::SmallVector<mlir::Value> results{returnStatus.value};
    if (currentResult_ != TypeId{0})
      results.push_back(value.value ? value.value : poison(location));
    builder_.create<mlir::func::ReturnOp>(location, results);
  }

  void emitPanic(mlir::Location location, mlir::Value message) {
    builder_.create<ir::PanicOp>(location, currentContext_, message);
    release(message, TypeId{4}, location);
    cleanupAll();
    emitFunctionReturn(location, FunctionReturnStatus{status(5)}, {});
  }

  mlir::Value propagateCallStatus(mlir::Location location,
                                  mlir::func::CallOp call,
                                  TypeId resultType) {
    mlir::Value callStatus = call.getResult(0);
    auto zero = builder_.create<mlir::arith::ConstantIntOp>(location, 0, 32);
    auto succeeded = builder_.create<mlir::arith::CmpIOp>(
        location, mlir::arith::CmpIPredicate::eq, callStatus, zero);
    auto* region = builder_.getInsertionBlock()->getParent();
    auto* failed = new mlir::Block;
    auto* continuation = new mlir::Block;
    const bool hasResult = resultType != TypeId{0};
    if (hasResult)
      continuation->addArgument(type(resultType), location);
    region->push_back(failed);
    region->push_back(continuation);
    builder_.create<mlir::cf::CondBranchOp>(
        location, succeeded, continuation,
        hasResult ? mlir::ValueRange{call.getResult(1)} : mlir::ValueRange{},
        failed, mlir::ValueRange{});
    builder_.setInsertionPointToStart(failed);
    cleanupAll();
    emitFunctionReturn(location, FunctionReturnStatus{callStatus}, {});
    builder_.setInsertionPointToStart(continuation);
    return hasResult ? continuation->getArgument(0) : mlir::Value{};
  }

  [[nodiscard]] bool terminated() const {
    return !builder_.getInsertionBlock()->empty() &&
           builder_.getInsertionBlock()
               ->back()
               .hasTrait<mlir::OpTrait::IsTerminator>();
  }

  mlir::Value runtime(mlir::Location location, llvm::StringRef callee,
                      TypeId resultType, mlir::ValueRange inputs = {},
                      llvm::ArrayRef<int32_t> metadata = {},
                      std::optional<TypeId> identity = std::nullopt) {
    llvm::SmallVector<mlir::Type> results;
    if (resultType != TypeId{0} && resultType != TypeId{5})
      results.push_back(type(resultType));
    const auto runtimeOperation = ir::parseRuntimeOperation(callee);
    assert(runtimeOperation.has_value());
    auto operation = builder_.create<ir::RuntimeOp>(
        location, results,
        ir::describeRuntimeOperation(*runtimeOperation).name,
        identity ? typeId(*identity).value() : 0,
        metadata, inputs);
    return operation.getNumResults() ? operation.getResult(0) : mlir::Value{};
  }

  mlir::Value readLocal(LocalId id, mlir::Location location) {
    const auto& local = environment_.get(id);
    mlir::Value value = local.value;
    if (local.address) {
      value =
          builder_.create<ir::SlotLoadOp>(location, type(local.type), value);
    }
    if (semantics_.typeTable.runtimeBacked(local.type))
      value = builder_.create<ir::RetainOp>(location, type(local.type), value);
    return value;
  }

  void release(mlir::Value value, TypeId valueType, mlir::Location location) {
    if (value && semantics_.typeTable.runtimeBacked(valueType))
      builder_.create<ir::ReleaseOp>(location, value);
  }

  void releaseLocal(LocalId id, mlir::Location location) {
    const auto& local = environment_.get(id);
    if (!local.owned || !semantics_.typeTable.runtimeBacked(local.type))
      return;
    mlir::Value value = local.value;
    if (local.address) {
      value =
          builder_.create<ir::SlotLoadOp>(location, type(local.type), value);
    }
    builder_.create<ir::ReleaseOp>(location, value);
  }

  void cleanupScope(mlir::Location location) {
    environment_.cleanupCurrent(
        [&](LocalId id) { releaseLocal(id, location); });
  }

  void cleanupAll() {
    environment_.cleanupAll(
        [&](LocalId id) { releaseLocal(id, builder_.getUnknownLoc()); });
  }

  std::optional<mlir::Value> emitBlock(const Block& block, bool nested = true) {
    if (nested)
      environment_.enterScope();
    for (const auto& statement : block.statements) {
      if (terminated())
        break;
      emitStatement(statement);
    }
    std::optional<mlir::Value> result;
    if (block.tail && !terminated()) {
      mlir::Value emitted = emitExpr(*block.tail);
      if (emitted)
        result = emitted;
    }
    if (nested && !terminated()) {
      cleanupScope(loc(block.location));
      environment_.leaveScope();
    } else if (nested) {
      environment_.leaveScope();
    }
    return result;
  }

  void bindLocal(LocalId id, mlir::Value initial, TypeId declared,
                 bool mutableBinding, mlir::Location location) {
    if (!mutableBinding) {
      environment_.bind(id, LocalValue{.value = initial,
                                       .type = declared,
                                       .address = false,
                                       .owned = true});
    } else {
      auto memoryType = ir::SlotType::get(&context_, type(declared));
      auto memory =
          builder_.create<ir::SlotAllocOp>(location, memoryType, initial);
      environment_.bind(id, LocalValue{.value = memory,
                                       .type = declared,
                                       .address = true,
                                       .owned = true});
    }
  }

  void emitStatement(const Statement& statement) {
    std::visit(
        Overloaded{
            [&](const BindingStmt& value) {
              mlir::Value initial = emitExpr(*value.initializer);
              if (terminated())
                return;
              const auto& ids = semantics_.bindings.at(&statement);
              TypeId declared =
                  value.annotation
                      ? semantics_.resolvedTypes.at(&*value.annotation)
                      : semantics_.expressionTypes.at(value.initializer->id);
              if (ids.size() == 1) {
                bindLocal(ids.front(), initial, declared, value.mutableBinding,
                          loc(statement.location));
                return;
              }
              const auto& tuple = semantics_.typeTable[declared];
              for (size_t i = 0; i < ids.size(); ++i) {
                mlir::Value element = runtime(
                    loc(statement.location), "tuple.get", tuple.arguments[i],
                    initial, {static_cast<int32_t>(i)});
                bindLocal(ids[i], element, tuple.arguments[i], false,
                          loc(statement.location));
              }
              release(initial, declared, loc(statement.location));
            },
            [&](const AssignmentStmt& value) {
              LocalId id = semantics_.assignments.at(&statement);
              auto& local = environment_.get(id);
              mlir::Value assigned = emitExpr(*value.value);
              if (terminated())
                return;
              releaseLocal(id, loc(statement.location));
              builder_.create<ir::SlotStoreOp>(loc(statement.location),
                                               assigned, local.value);
            },
            [&](const ExpressionStmt& value) {
              mlir::Value result = emitExpr(*value.value);
              if (!terminated())
                release(result, semantics_.expressionTypes.at(value.value->id),
                        loc(statement.location));
            }},
        statement.value);
  }

  mlir::Value emitExpr(const Expr& expression) {
    mlir::Location location = loc(expression.location);
    TypeId resultType = semantics_.expressionTypes.at(expression.id);
    return std::visit(
        Overloaded{
            [&](const LiteralExpr& value) -> mlir::Value {
              return std::visit(
                  Overloaded{
                      [](const std::monostate&) -> mlir::Value { return {}; },
                      [&](bool literal) -> mlir::Value {
                        return builder_.create<mlir::arith::ConstantIntOp>(
                            location, literal, 1);
                      },
                      [&](int64_t literal) -> mlir::Value {
                        return builder_.create<mlir::arith::ConstantIntOp>(
                            location, literal, 64);
                      },
                      [&](double literal) -> mlir::Value {
                        return builder_.create<mlir::arith::ConstantFloatOp>(
                            location, builder_.getF64Type(),
                            llvm::APFloat(literal));
                      },
                      [&](const std::string& literal) -> mlir::Value {
                        return builder_.create<ir::StringLiteralOp>(
                            location, ir::StringType::get(&context_),
                            builder_.getStringAttr(literal));
                      }},
                  value.value);
            },
            [&](const NameExpr&) -> mlir::Value {
              const auto& reference = semantics_.references.at(expression.id);
              if (const auto* option =
                      std::get_if<OptionConstructorReference>(&reference)) {
                if (option->some)
                  return {};
                return runtime(location, "enum.create", resultType, {}, {0},
                               resultType);
              }
              if (const auto* standard =
                      std::get_if<StandardErrorConstructorReference>(
                          &reference))
                return runtime(
                    location, "enum.create", resultType, {},
                    {static_cast<int32_t>(standard->variant.value())},
                    resultType);
              return readLocal(std::get<LocalReference>(reference).local,
                               location);
            },
            [&](const MemberExpr& value) -> mlir::Value {
              const auto& reference = semantics_.references.at(expression.id);
              if (const auto* variant =
                      std::get_if<EnumVariantReference>(&reference))
                return runtime(location, "enum.create", resultType, {},
                               {static_cast<int32_t>(variant->variant.value())},
                               resultType);
              mlir::Value base = emitExpr(*value.base);
              TypeId baseType = semantics_.expressionTypes.at(value.base->id);
              const bool tuple =
                  std::holds_alternative<TupleFieldReference>(reference);
              const std::size_t index =
                  tuple
                      ? std::get<TupleFieldReference>(reference).field.value()
                      : std::get<RecordFieldReference>(reference).field.value();
              mlir::Value result =
                  runtime(location, tuple ? "tuple.get" : "record.get",
                          resultType, base, {static_cast<int32_t>(index)});
              release(base, baseType, location);
              return result;
            },
            [&](const CallExpr& value) -> mlir::Value {
              return emitCall(expression, value);
            },
            [&](const SpawnExpr& value) -> mlir::Value {
              const auto& call = std::get<CallExpr>(value.call->value);
              llvm::SmallVector<mlir::Value> arguments;
              for (const auto& argument : call.arguments)
                arguments.push_back(emitExpr(*argument));
              const auto function = std::get<SpawnReference>(
                                        semantics_.references.at(expression.id))
                                        .function;
              auto target = functions_.at(function);
              auto spawned = builder_.create<ir::TaskSpawnOp>(
                  location, type(resultType), currentContext_,
                  mlir::FlatSymbolRefAttr::get(target.getSymNameAttr()),
                  arguments);
              for (std::size_t index = 0; index < arguments.size(); ++index)
                release(
                    arguments[index],
                    semantics_.expressionTypes.at(call.arguments[index]->id),
                    location);
              return spawned;
            },
            [&](const UnaryExpr& value) -> mlir::Value {
              auto operand = emitExpr(*value.operand);
              TypeId operandType =
                  semantics_.expressionTypes.at(value.operand->id);
              if (value.op == "!") {
                auto one =
                    builder_.create<mlir::arith::ConstantIntOp>(location, 1, 1);
                return builder_.create<mlir::arith::XOrIOp>(location, operand,
                                                            one);
              }
              if (semantics_.typeTable[operandType].kind == TypeKind::Int) {
                auto zero = builder_.create<mlir::arith::ConstantIntOp>(
                    location, 0, 64);
                return builder_.create<mlir::arith::SubIOp>(location, zero,
                                                            operand);
              }
              return builder_.create<mlir::arith::NegFOp>(location, operand);
            },
            [&](const BinaryExpr& value) -> mlir::Value {
              return emitBinary(expression, value);
            },
            [&](const IfExpr& value) -> mlir::Value {
              return emitIf(expression, value);
            },
            [&](const BlockExpr& value) -> mlir::Value {
              return emitBlock(*value.block).value_or(mlir::Value{});
            },
            [&](const TupleExpr& value) -> mlir::Value {
              llvm::SmallVector<mlir::Value> elements;
              for (const auto& element : value.elements)
                elements.push_back(emitExpr(*element));
              auto result = runtime(location, "tuple.create", resultType,
                                    elements, {}, resultType);
              for (size_t i = 0; i < elements.size(); ++i)
                release(elements[i],
                        semantics_.expressionTypes.at(value.elements[i]->id),
                        location);
              return result;
            },
            [&](const ListExpr& value) -> mlir::Value {
              mlir::Value list = runtime(location, "list.create", resultType,
                                         {}, {}, resultType);
              for (const auto& element : value.elements) {
                mlir::Value item = emitExpr(*element);
                mlir::Value next = runtime(location, "list.push", resultType,
                                           {list, item}, {}, resultType);
                release(list, resultType, location);
                release(item, semantics_.expressionTypes.at(element->id),
                        location);
                list = next;
              }
              return list;
            },
            [&](const GenericConstructorExpr&) -> mlir::Value {
              const auto& constructed = semantics_.typeTable[resultType];
              if (constructed.kind == TypeKind::List)
                return runtime(location, "list.create", resultType, {}, {},
                               resultType);
              const int32_t keyKind =
                  semantics_.typeTable[constructed.arguments[0]].kind ==
                          TypeKind::String
                      ? 1
                      : 0;
              return runtime(location, "map.create", resultType, {}, {keyKind},
                             resultType);
            },
            [&](const RecordExpr& value) -> mlir::Value {
              const auto& reference = std::get<RecordConstructorReference>(
                  semantics_.references.at(expression.id));
              const auto& declaration =
                  at(at(modules_, reference.record.module).records,
                     reference.record.record);
              llvm::SmallVector<mlir::Value> fields(declaration.fields.size());
              llvm::SmallVector<TypeId> fieldTypes(declaration.fields.size(),
                                                   TypeId{0});
              for (const auto& field : value.fields) {
                auto found = std::ranges::find(declaration.fields, field.name,
                                               &RecordFieldDecl::name);
                size_t index = found - declaration.fields.begin();
                fields[index] = emitExpr(*field.value);
                fieldTypes[index] =
                    semantics_.expressionTypes.at(field.value->id);
              }
              auto result = runtime(location, "record.create", resultType,
                                    fields, {}, resultType);
              for (size_t i = 0; i < fields.size(); ++i)
                release(fields[i], fieldTypes[i], location);
              return result;
            },
            [&](const RecordUpdateExpr& value) -> mlir::Value {
              mlir::Value current = emitExpr(*value.base);
              TypeId currentType =
                  semantics_.expressionTypes.at(value.base->id);
              const auto& semantic = semantics_.typeTable[currentType];
              if (!semantic.declaration)
                return current;
              const RecordRef recordReference =
                  std::get<RecordRef>(semantic.declaration.value());
              const auto& declaration =
                  at(at(modules_, recordReference.module).records,
                     recordReference.record);
              for (const auto& field : value.fields) {
                auto found = std::ranges::find(declaration.fields, field.name,
                                               &RecordFieldDecl::name);
                int32_t index =
                    static_cast<int32_t>(found - declaration.fields.begin());
                mlir::Value replacement = emitExpr(*field.value);
                mlir::Value next =
                    runtime(location, "record.with", resultType,
                            {current, replacement}, {index}, resultType);
                release(current, currentType, location);
                release(replacement,
                        semantics_.expressionTypes.at(field.value->id),
                        location);
                current = next;
              }
              return current;
            },
            [&](const TryExpr& value) -> mlir::Value {
              return emitTry(expression, value);
            },
            [&](const RangeExpr&) -> mlir::Value { return {}; },
            [&](const MatchExpr& value) -> mlir::Value {
              return emitMatch(expression, value);
            },
            [&](const WhileExpr& value) -> mlir::Value {
              emitWhile(expression, value);
              return {};
            },
            [&](const ForExpr& value) -> mlir::Value {
              emitFor(expression, value);
              return {};
            },
            [&](const LoopExpr& value) -> mlir::Value {
              emitLoop(expression, value);
              return {};
            },
            [&](const ControlExpr& value) -> mlir::Value {
              emitControl(expression, value);
              return {};
            }},
        expression.value);
  }

  mlir::Value emitCall(const Expr& expression, const CallExpr& value) {
    mlir::Location location = loc(expression.location);
    TypeId resultType = semantics_.expressionTypes.at(expression.id);
    const auto& reference = semantics_.references.at(value.callee->id);
    if (const auto* methodReference =
            std::get_if<BuiltinMethodReference>(&reference)) {
      const auto& member = std::get<MemberExpr>(value.callee->value);
      llvm::SmallVector<mlir::Value> inputs{emitExpr(*member.base)};
      for (const auto& argument : value.arguments)
        inputs.push_back(emitExpr(*argument));
      static const std::unordered_map<BuiltinMethod, llvm::StringLiteral>
          names = {
              {BuiltinMethod::StringLength, "string.length"},
              {BuiltinMethod::IntToString, "int.to_string"},
              {BuiltinMethod::StringSplitOnce, "string.split_once"},
              {BuiltinMethod::ListLength, "list.length"},
              {BuiltinMethod::ListIsEmpty, "list.is_empty"},
              {BuiltinMethod::ListGet, "list.get"},
              {BuiltinMethod::ListPush, "list.push"},
              {BuiltinMethod::ListSet, "list.set"},
              {BuiltinMethod::ListRemoveAt, "list.remove_at"},
              {BuiltinMethod::ListTakeLast, "list.take_last"},
              {BuiltinMethod::MapLength, "map.length"},
              {BuiltinMethod::MapIsEmpty, "map.is_empty"},
              {BuiltinMethod::MapContains, "map.contains"},
              {BuiltinMethod::MapGet, "map.get"},
              {BuiltinMethod::MapInsert, "map.insert"},
              {BuiltinMethod::MapRemove, "map.remove"},
              {BuiltinMethod::MapEntries, "map.entries"},
              {BuiltinMethod::SenderSend, "channel.send"},
              {BuiltinMethod::SenderClose, "channel.close"},
              {BuiltinMethod::ReceiverReceive, "channel.receive"},
              {BuiltinMethod::TaskJoin, "task.join"},
              {BuiltinMethod::ListenerAccept, "transport.accept"},
              {BuiltinMethod::ListenerLocalAddress, "transport.local_address"},
              {BuiltinMethod::ListenerClose, "transport.close"},
              {BuiltinMethod::ConnectionSend, "transport.send"},
              {BuiltinMethod::ConnectionReceive, "transport.receive"},
              {BuiltinMethod::ConnectionClose, "transport.close"}};
      llvm::SmallVector<int32_t> metadata;
      if (methodReference->method == BuiltinMethod::StringSplitOnce) {
        const auto& option = semantics_.typeTable[resultType];
        metadata.push_back(
            static_cast<int32_t>(typeId(option.arguments.front()).value()));
      } else if (methodReference->method == BuiltinMethod::MapEntries) {
        const auto& list = semantics_.typeTable[resultType];
        metadata.push_back(
            static_cast<int32_t>(typeId(list.arguments.front()).value()));
      }
      mlir::Value result = runtime(location, names.at(methodReference->method),
                                   resultType, inputs, metadata, resultType);
      release(inputs.front(), semantics_.expressionTypes.at(member.base->id),
              location);
      for (size_t i = 0; i < value.arguments.size(); ++i)
        release(inputs[i + 1],
                semantics_.expressionTypes.at(value.arguments[i]->id),
                location);
      return result;
    }
    llvm::SmallVector<mlir::Value> arguments;
    llvm::SmallVector<TypeId> argumentTypes;
    for (const auto& argument : value.arguments) {
      mlir::Value emitted = emitExpr(*argument);
      if (emitted) {
        arguments.push_back(emitted);
        argumentTypes.push_back(semantics_.expressionTypes.at(argument->id));
      }
    }
    if (const auto* output = std::get_if<OutputReference>(&reference)) {
      if (output->newline)
        builder_.create<ir::PrintlnOp>(location, arguments.front());
      else
        builder_.create<ir::PrintOp>(location, arguments.front());
      release(arguments.front(),
              semantics_.expressionTypes.at(value.arguments[0]->id), location);
      return {};
    }
    if (const auto* builtin =
            std::get_if<BuiltinFunctionReference>(&reference)) {
      if (builtin->function == BuiltinFunction::Panic) {
        emitPanic(location, arguments.front());
        return {};
      }
      llvm::StringRef name;
      switch (builtin->function) {
      case BuiltinFunction::Print:
      case BuiltinFunction::Println:
        return {};
      case BuiltinFunction::ReadLine:
        name = "io.read_line";
        break;
      case BuiltinFunction::ReadText:
        name = "io.read_text";
        break;
      case BuiltinFunction::WriteText:
        name = "io.write_text";
        break;
      case BuiltinFunction::UnixSeconds:
        name = "time.unix_seconds";
        break;
      case BuiltinFunction::SleepMilliseconds:
        name = "time.sleep_ms";
        break;
      case BuiltinFunction::ParseInt:
        name = "int.parse";
        break;
      case BuiltinFunction::Panic:
        llvm_unreachable("panic is handled before runtime dispatch");
      case BuiltinFunction::ChannelCreate:
        name = "channel.create";
        break;
      case BuiltinFunction::TransportListen:
        name = "transport.listen";
        break;
      case BuiltinFunction::TransportConnect:
        name = "transport.connect";
        break;
      }
      if (builtin->function == BuiltinFunction::TransportListen ||
          builtin->function == BuiltinFunction::TransportConnect) {
        const auto& result = semantics_.typeTable[resultType];
        if (!result.arguments.empty()) {
          const auto& resource = semantics_.typeTable[result.arguments.front()];
          for (TypeId messageType : resource.arguments) {
            const auto hash = typeId(messageType).value();
            arguments.push_back(builder_.create<mlir::arith::ConstantIntOp>(
                location, static_cast<int64_t>(hash), 64));
            argumentTypes.push_back(TypeId{2});
          }
        }
      }
      mlir::Value result =
          runtime(location, name, resultType, arguments, {}, resultType);
      for (size_t i = 0; i < arguments.size(); ++i)
        release(arguments[i], argumentTypes[i], location);
      return result;
    }
    if (std::holds_alternative<OptionConstructorReference>(reference) ||
        std::holds_alternative<ResultConstructorReference>(reference) ||
        std::holds_alternative<EnumVariantReference>(reference) ||
        std::holds_alternative<StandardErrorConstructorReference>(reference)) {
      int32_t tag = std::visit(
          Overloaded{[](const OptionConstructorReference& constructor) {
                       return constructor.some ? 1 : 0;
                     },
                     [](const ResultConstructorReference& constructor) {
                       return constructor.ok ? 0 : 1;
                     },
                     [](const EnumVariantReference& variant) {
                       return static_cast<int32_t>(variant.variant.value());
                     },
                     [](const StandardErrorConstructorReference& variant) {
                       return static_cast<int32_t>(variant.variant.value());
                     },
                     [](const auto&) { return 0; }},
          reference);
      mlir::Value result = runtime(location, "enum.create", resultType,
                                   arguments, {tag}, resultType);
      for (size_t i = 0; i < arguments.size(); ++i)
        release(arguments[i], argumentTypes[i], location);
      return result;
    }
    const auto function = std::get<FunctionReference>(reference).function;
    auto target = functions_.at(function);
    llvm::SmallVector<mlir::Type> results{builder_.getI32Type()};
    if (resultType != TypeId{0})
      results.push_back(type(resultType));
    arguments.insert(arguments.begin(), currentContext_);
    auto call = builder_.create<mlir::func::CallOp>(
        location, target.getSymName(), results, arguments);
    for (size_t i = 0; i < argumentTypes.size(); ++i)
      release(arguments[i + 1], argumentTypes[i], location);
    return propagateCallStatus(location, call, resultType);
  }

  mlir::Value emitBinary(const Expr& expression, const BinaryExpr& value) {
    mlir::Location location = loc(expression.location);
    mlir::Value left = emitExpr(*value.left);
    if (value.op == "&&" || value.op == "||") {
      auto* region = builder_.getInsertionBlock()->getParent();
      auto* rightBlock = new mlir::Block;
      auto* shortCircuitBlock = new mlir::Block;
      auto* continuation = new mlir::Block;
      continuation->addArgument(builder_.getI1Type(), location);
      region->push_back(rightBlock);
      region->push_back(shortCircuitBlock);
      region->push_back(continuation);
      builder_.create<mlir::cf::CondBranchOp>(
          location, left,
          value.op == "&&" ? rightBlock : shortCircuitBlock,
          mlir::ValueRange{},
          value.op == "&&" ? shortCircuitBlock : rightBlock,
          mlir::ValueRange{});
      builder_.setInsertionPointToStart(rightBlock);
      mlir::Value right = emitExpr(*value.right);
      if (!terminated())
        builder_.create<mlir::cf::BranchOp>(location, continuation, right);
      builder_.setInsertionPointToStart(shortCircuitBlock);
      auto shortCircuitValue = builder_.create<mlir::arith::ConstantIntOp>(
          location, value.op == "||", 1);
      builder_.create<mlir::cf::BranchOp>(location, continuation,
                                          shortCircuitValue.getResult());
      builder_.setInsertionPointToStart(continuation);
      return continuation->getArgument(0);
    }
    mlir::Value right = emitExpr(*value.right);
    TypeId operand = semantics_.expressionTypes.at(value.left->id);
    TypeKind kind = semantics_.typeTable[operand].kind;
    if (kind == TypeKind::String && value.op == "+") {
      mlir::Value result = runtime(location, "string.concat", operand,
                                   {left, right}, {}, operand);
      release(left, operand, location);
      release(right, operand, location);
      return result;
    }
    if ((value.op == "==" || value.op == "!=") &&
        semantics_.typeTable.runtimeBacked(operand)) {
      mlir::Value equal = builder_.create<ir::ValueEqualOp>(
          location, builder_.getI1Type(), left, right);
      release(left, operand, location);
      release(right, operand, location);
      if (value.op == "==")
        return equal;
      auto one = builder_.create<mlir::arith::ConstantIntOp>(location, 1, 1);
      return builder_.create<mlir::arith::XOrIOp>(location, equal, one);
    }
    if (kind == TypeKind::Float) {
      if (value.op == "+")
        return builder_.create<mlir::arith::AddFOp>(location, left, right);
      if (value.op == "-")
        return builder_.create<mlir::arith::SubFOp>(location, left, right);
      if (value.op == "*")
        return builder_.create<mlir::arith::MulFOp>(location, left, right);
      if (value.op == "/")
        return builder_.create<mlir::arith::DivFOp>(location, left, right);
      mlir::arith::CmpFPredicate predicate = mlir::arith::CmpFPredicate::OEQ;
      if (value.op == "!=")
        predicate = mlir::arith::CmpFPredicate::UNE;
      else if (value.op == "<")
        predicate = mlir::arith::CmpFPredicate::OLT;
      else if (value.op == "<=")
        predicate = mlir::arith::CmpFPredicate::OLE;
      else if (value.op == ">")
        predicate = mlir::arith::CmpFPredicate::OGT;
      else if (value.op == ">=")
        predicate = mlir::arith::CmpFPredicate::OGE;
      return builder_.create<mlir::arith::CmpFOp>(location, predicate, left,
                                                  right);
    }
    if (value.op == "+")
      return builder_.create<mlir::arith::AddIOp>(location, left, right);
    if (value.op == "-")
      return builder_.create<mlir::arith::SubIOp>(location, left, right);
    if (value.op == "*")
      return builder_.create<mlir::arith::MulIOp>(location, left, right);
    if (value.op == "/" || value.op == "%")
      return emitCheckedDivision(location, left, right, value.op == "%");
    mlir::arith::CmpIPredicate predicate = mlir::arith::CmpIPredicate::eq;
    if (value.op == "!=")
      predicate = mlir::arith::CmpIPredicate::ne;
    else if (value.op == "<")
      predicate = mlir::arith::CmpIPredicate::slt;
    else if (value.op == "<=")
      predicate = mlir::arith::CmpIPredicate::sle;
    else if (value.op == ">")
      predicate = mlir::arith::CmpIPredicate::sgt;
    else if (value.op == ">=")
      predicate = mlir::arith::CmpIPredicate::sge;
    return builder_.create<mlir::arith::CmpIOp>(location, predicate, left,
                                                right);
  }

  mlir::Value emitCheckedDivision(mlir::Location location, mlir::Value left,
                                  mlir::Value right, bool remainder) {
    auto zero = builder_.create<mlir::arith::ConstantIntOp>(location, 0, 64);
    auto isZero = builder_.create<mlir::arith::CmpIOp>(
        location, mlir::arith::CmpIPredicate::eq, right, zero);
    auto minimum = builder_.create<mlir::arith::ConstantIntOp>(
        location, std::numeric_limits<int64_t>::min(), 64);
    auto negativeOne =
        builder_.create<mlir::arith::ConstantIntOp>(location, -1, 64);
    auto leftMin = builder_.create<mlir::arith::CmpIOp>(
        location, mlir::arith::CmpIPredicate::eq, left, minimum);
    auto rightNegativeOne = builder_.create<mlir::arith::CmpIOp>(
        location, mlir::arith::CmpIPredicate::eq, right, negativeOne);
    auto overflow = builder_.create<mlir::arith::AndIOp>(location, leftMin,
                                                         rightNegativeOne);
    auto invalid =
        builder_.create<mlir::arith::OrIOp>(location, isZero, overflow);
    auto* region = builder_.getInsertionBlock()->getParent();
    auto* failed = new mlir::Block;
    auto* valid = new mlir::Block;
    region->push_back(failed);
    region->push_back(valid);
    builder_.create<mlir::cf::CondBranchOp>(location, invalid, failed,
                                            mlir::ValueRange{}, valid,
                                            mlir::ValueRange{});
    builder_.setInsertionPointToStart(failed);
    auto message = builder_.create<ir::StringLiteralOp>(
        location, ir::StringType::get(&context_),
        builder_.getStringAttr("invalid integer division"));
    emitPanic(location, message);
    builder_.setInsertionPointToStart(valid);
    mlir::Value result =
        remainder
            ? mlir::Value(
                  builder_.create<mlir::arith::RemSIOp>(location, left, right))
            : mlir::Value(
                  builder_.create<mlir::arith::DivSIOp>(location, left, right));
    return result;
  }

  mlir::Value emitIf(const Expr& expression, const IfExpr& value) {
    mlir::Location location = loc(expression.location);
    mlir::Value condition = emitExpr(*value.condition);
    TypeId resultType = semantics_.expressionTypes.at(expression.id);
    const bool branchReturns =
        resultType == TypeId{0} ||
        blockContainsControlTransfer(*value.thenBlock) ||
        std::visit(Overloaded{[](const std::monostate&) { return false; },
                              [](const std::unique_ptr<Block>& branch) {
                                return blockContainsControlTransfer(*branch);
                              },
                              [](const ExprPtr& branch) {
                                return expressionContainsControlTransfer(
                                    *branch);
                              }},
                   value.elseBranch);
    if (branchReturns) {
      auto* region = builder_.getInsertionBlock()->getParent();
      auto* thenBlock = new mlir::Block;
      auto* elseBlock = new mlir::Block;
      auto* continuation = new mlir::Block;
      const bool hasResult = resultType != TypeId{0} && resultType != TypeId{5};
      if (hasResult)
        continuation->addArgument(type(resultType), location);
      region->push_back(thenBlock);
      region->push_back(elseBlock);
      region->push_back(continuation);
      builder_.create<mlir::cf::CondBranchOp>(location, condition, thenBlock,
                                              mlir::ValueRange{}, elseBlock,
                                              mlir::ValueRange{});
      builder_.setInsertionPointToStart(thenBlock);
      auto thenValue = emitBlock(*value.thenBlock);
      const bool thenTerminated = terminated();
      if (!thenTerminated)
        builder_.create<mlir::cf::BranchOp>(
            location, continuation,
            thenValue ? mlir::ValueRange{*thenValue} : mlir::ValueRange{});
      builder_.setInsertionPointToStart(elseBlock);
      auto elseValue = std::visit(
          Overloaded{[](const std::monostate&) -> std::optional<mlir::Value> {
                       return std::nullopt;
                     },
                     [&](const std::unique_ptr<Block>& branch) {
                       return emitBlock(*branch);
                     },
                     [&](const ExprPtr& branch) -> std::optional<mlir::Value> {
                       mlir::Value result = emitExpr(*branch);
                       return result ? std::optional<mlir::Value>{result}
                                     : std::nullopt;
                     }},
          value.elseBranch);
      const bool elseTerminated = terminated();
      if (!elseTerminated)
        builder_.create<mlir::cf::BranchOp>(
            location, continuation,
            elseValue ? mlir::ValueRange{*elseValue} : mlir::ValueRange{});
      if (thenTerminated && elseTerminated) {
        continuation->erase();
        builder_.setInsertionPoint(thenBlock->getTerminator());
        return {};
      }
      builder_.setInsertionPointToStart(continuation);
      return hasResult ? continuation->getArgument(0) : mlir::Value{};
    }
    llvm::SmallVector<mlir::Type> results;
    if (resultType != TypeId{0} && resultType != TypeId{5})
      results.push_back(type(resultType));
    bool hasElse = !std::holds_alternative<std::monostate>(value.elseBranch);
    auto operation =
        builder_.create<mlir::scf::IfOp>(location, results, condition, hasElse);
    auto prepare = [&](mlir::Block& block) {
      if (!block.empty() && mlir::isa<mlir::scf::YieldOp>(block.back()))
        block.back().erase();
      builder_.setInsertionPointToStart(&block);
    };
    prepare(operation.getThenRegion().front());
    auto thenValue = emitBlock(*value.thenBlock);
    if (!terminated())
      builder_.create<mlir::scf::YieldOp>(
          location,
          thenValue ? mlir::ValueRange{*thenValue} : mlir::ValueRange{});
    if (hasElse) {
      prepare(operation.getElseRegion().front());
      std::optional<mlir::Value> elseValue = std::visit(
          Overloaded{[](const std::monostate&) -> std::optional<mlir::Value> {
                       return std::nullopt;
                     },
                     [&](const std::unique_ptr<Block>& branch) {
                       return emitBlock(*branch);
                     },
                     [&](const ExprPtr& branch) -> std::optional<mlir::Value> {
                       mlir::Value result = emitExpr(*branch);
                       return result ? std::optional<mlir::Value>{result}
                                     : std::nullopt;
                     }},
          value.elseBranch);
      if (!terminated())
        builder_.create<mlir::scf::YieldOp>(
            location,
            elseValue ? mlir::ValueRange{*elseValue} : mlir::ValueRange{});
    }
    builder_.setInsertionPointAfter(operation);
    return operation.getNumResults() ? operation.getResult(0) : mlir::Value{};
  }

  mlir::Value emitTry(const Expr& expression, const TryExpr& value) {
    mlir::Location location = loc(expression.location);
    mlir::Value source = emitExpr(*value.value);
    TypeId sourceType = semantics_.expressionTypes.at(value.value->id);
    TypeId resultType = semantics_.expressionTypes.at(expression.id);
    mlir::Value tag = runtime(location, "enum.tag", TypeId{2}, source);
    auto zero = builder_.create<mlir::arith::ConstantIntOp>(location, 0, 64);
    auto ok = builder_.create<mlir::arith::CmpIOp>(
        location, mlir::arith::CmpIPredicate::eq, tag, zero);
    auto* region = builder_.getInsertionBlock()->getParent();
    auto* success = new mlir::Block;
    auto* failure = new mlir::Block;
    auto* continuation = new mlir::Block;
    const bool hasResult = resultType != TypeId{0};
    if (hasResult)
      continuation->addArgument(type(resultType), location);
    region->push_back(success);
    region->push_back(failure);
    region->push_back(continuation);
    builder_.create<mlir::cf::CondBranchOp>(
        location, ok, success, mlir::ValueRange{}, failure, mlir::ValueRange{});
    builder_.setInsertionPointToStart(failure);
    TypeId errorType = semantics_.typeTable[sourceType].arguments[1];
    mlir::Value error =
        runtime(location, "enum.payload", errorType, source, {0});
    TypeId returnType =
        semantics_.functions
            .at(at(at(modules_, currentFunction_.module).functions,
                   currentFunction_.function)
                    .id)
            .result;
    mlir::Value returned =
        runtime(location, "enum.create", returnType, error, {1}, returnType);
    release(error, errorType, location);
    release(source, sourceType, location);
    cleanupAll();
    emitFunctionReturn(location, FunctionReturnStatus{status(0)},
                       FunctionReturnValue{returned});
    builder_.setInsertionPointToStart(success);
    mlir::Value unwrapped;
    if (hasResult)
      unwrapped = runtime(location, "enum.payload", resultType, source, {0});
    release(source, sourceType, location);
    builder_.create<mlir::cf::BranchOp>(location, continuation,
                                        hasResult ? mlir::ValueRange{unwrapped}
                                                  : mlir::ValueRange{});
    builder_.setInsertionPointToStart(continuation);
    return hasResult ? continuation->getArgument(0) : mlir::Value{};
  }

  [[nodiscard]] std::vector<TypeId>
  patternPayloadTypes(const Pattern& pattern, TypeId valueType) const {
    const auto& semantic = semantics_.typeTable[valueType];
    if (semantic.kind == TypeKind::Option)
      return constructorTag(semantics_.patterns.at(pattern.id)) == 1
                 ? std::vector<TypeId>{semantic.arguments[0]}
                 : std::vector<TypeId>{};
    if (semantic.kind == TypeKind::Result)
      return {semantic.arguments.at(
          constructorTag(semantics_.patterns.at(pattern.id)))};
    const std::size_t tag = constructorTag(semantics_.patterns.at(pattern.id));
    if (semantic.kind == TypeKind::IndexError ||
        semantic.kind == TypeKind::ChannelError ||
        semantic.kind == TypeKind::ParseIntError)
      return {};
    if (semantic.kind == TypeKind::TaskError)
      return tag == 0 ? std::vector<TypeId>{TypeId{4}} : std::vector<TypeId>{};
    if (semantic.kind == TypeKind::IoError)
      return tag == 3 ? std::vector<TypeId>{TypeId{4}} : std::vector<TypeId>{};
    if (semantic.kind == TypeKind::TransportError)
      return tag == 6 ? std::vector<TypeId>{TypeId{4}} : std::vector<TypeId>{};
    if (!semantic.declaration)
      return {};
    const EnumRef enumReference =
        std::get<EnumRef>(semantic.declaration.value());
    const auto& declaration =
        at(at(modules_, enumReference.module).enums, enumReference.enumeration);
    return semantics_.enums.at(declaration.id)
        .variants.at(constructorTag(semantics_.patterns.at(pattern.id)));
  }

  mlir::Value patternCondition(const Pattern& pattern, TypeId valueType,
                               mlir::Value value, mlir::Location location) {
    return std::visit(
        Overloaded{
            [&](const WildcardPattern&) -> mlir::Value {
              return builder_.create<mlir::arith::ConstantIntOp>(location, 1,
                                                                 1);
            },
            [&](const BindingPattern&) -> mlir::Value {
              return builder_.create<mlir::arith::ConstantIntOp>(location, 1,
                                                                 1);
            },
            [&](const LiteralPattern& literal) -> mlir::Value {
              return std::visit(
                  Overloaded{
                      [&](bool item) -> mlir::Value {
                        auto constant =
                            builder_.create<mlir::arith::ConstantIntOp>(
                                location, item, 1);
                        return builder_.create<mlir::arith::CmpIOp>(
                            location, mlir::arith::CmpIPredicate::eq, value,
                            constant);
                      },
                      [&](int64_t item) -> mlir::Value {
                        auto constant =
                            builder_.create<mlir::arith::ConstantIntOp>(
                                location, item, 64);
                        return builder_.create<mlir::arith::CmpIOp>(
                            location, mlir::arith::CmpIPredicate::eq, value,
                            constant);
                      },
                      [&](const std::string& item) -> mlir::Value {
                        auto constant = builder_.create<ir::StringLiteralOp>(
                            location, ir::StringType::get(&context_),
                            builder_.getStringAttr(item));
                        auto equal = builder_.create<ir::ValueEqualOp>(
                            location, builder_.getI1Type(), value, constant);
                        builder_.create<ir::ReleaseOp>(location, constant);
                        return equal;
                      }},
                  literal.value);
            },
            [&](const ConstructorPattern&) -> mlir::Value {
              const auto& reference = semantics_.patterns.at(pattern.id);
              mlir::Value tag = runtime(location, "enum.tag", TypeId{2}, value);
              auto expected = builder_.create<mlir::arith::ConstantIntOp>(
                  location, constructorTag(reference), 64);
              mlir::Value tagMatches = builder_.create<mlir::arith::CmpIOp>(
                  location, mlir::arith::CmpIPredicate::eq, tag, expected);
              const auto& constructor =
                  std::get<ConstructorPattern>(pattern.value);
              if (constructor.arguments.empty())
                return tagMatches;
              auto operation = builder_.create<mlir::scf::IfOp>(
                  location, builder_.getI1Type(), tagMatches, true);
              builder_.setInsertionPointToStart(
                  &operation.getThenRegion().front());
              mlir::Value matches =
                  builder_.create<mlir::arith::ConstantIntOp>(location, 1, 1);
              const auto payloadTypes = patternPayloadTypes(pattern, valueType);
              for (size_t index = 0; index < constructor.arguments.size();
                   ++index) {
                mlir::Value payload;
                if (payloadTypes[index] != TypeId{0})
                  payload =
                      runtime(location, "enum.payload", payloadTypes[index],
                              value, {static_cast<int32_t>(index)});
                mlir::Value nested =
                    patternCondition(*constructor.arguments[index],
                                     payloadTypes[index], payload, location);
                matches = builder_.create<mlir::arith::AndIOp>(location,
                                                               matches, nested);
                release(payload, payloadTypes[index], location);
              }
              builder_.create<mlir::scf::YieldOp>(location,
                                                  mlir::ValueRange{matches});
              builder_.setInsertionPointToStart(
                  &operation.getElseRegion().front());
              auto no =
                  builder_.create<mlir::arith::ConstantIntOp>(location, 0, 1);
              builder_.create<mlir::scf::YieldOp>(
                  location, mlir::ValueRange{no.getResult()});
              builder_.setInsertionPointAfter(operation);
              return operation.getResult(0);
            }},
        pattern.value);
  }

  void bindPatternValues(const Pattern& pattern, TypeId valueType,
                         mlir::Value value, mlir::Location location) {
    std::visit(
        Overloaded{[&](const WildcardPattern&) {},
                   [&](const BindingPattern&) {
                     LocalId id = semantics_.patternBindings.at(pattern.id);
                     mlir::Value retained = value;
                     if (semantics_.typeTable.runtimeBacked(valueType))
                       retained = builder_.create<ir::RetainOp>(
                           location, type(valueType), value);
                     bindLocal(id, retained, valueType, false, location);
                   },
                   [&](const LiteralPattern&) {},
                   [&](const ConstructorPattern& constructor) {
                     const auto& semantic = semantics_.typeTable[valueType];
                     std::vector<TypeId> payloadTypes;
                     if (semantic.kind == TypeKind::Option)
                       payloadTypes =
                           constructor.name.front() == "Some"
                               ? std::vector<TypeId>{semantic.arguments[0]}
                               : std::vector<TypeId>{};
                     else if (semantic.kind == TypeKind::Result)
                       payloadTypes = {semantic.arguments.at(
                           constructorTag(semantics_.patterns.at(pattern.id)))};
                     else if (semantic.kind == TypeKind::IndexError ||
                              semantic.kind == TypeKind::ChannelError ||
                              semantic.kind == TypeKind::TaskError ||
                              semantic.kind == TypeKind::IoError ||
                              semantic.kind == TypeKind::TransportError ||
                              semantic.kind == TypeKind::ParseIntError)
                       payloadTypes = patternPayloadTypes(pattern, valueType);
                     else {
                       if (!semantic.declaration)
                         return;
                       const EnumRef enumReference =
                           std::get<EnumRef>(semantic.declaration.value());
                       const auto& declaration =
                           at(at(modules_, enumReference.module).enums,
                              enumReference.enumeration);
                       payloadTypes =
                           semantics_.enums.at(declaration.id)
                               .variants.at(constructorTag(
                                   semantics_.patterns.at(pattern.id)));
                     }
                     for (size_t i = 0; i < constructor.arguments.size() &&
                                        i < payloadTypes.size();
                          ++i) {
                       mlir::Value payload;
                       if (payloadTypes[i] != TypeId{0})
                         payload = runtime(location, "enum.payload",
                                           payloadTypes[i], value,
                                           {static_cast<int32_t>(i)});
                       bindPatternValues(*constructor.arguments[i],
                                         payloadTypes[i], payload, location);
                       release(payload, payloadTypes[i], location);
                     }
                   }},
        pattern.value);
  }

  mlir::Value emitMatch(const Expr& expression, const MatchExpr& match) {
    mlir::Location location = loc(expression.location);
    mlir::Value value = emitExpr(*match.value);
    TypeId valueType = semantics_.expressionTypes.at(match.value->id);
    TypeId resultType = semantics_.expressionTypes.at(expression.id);
    auto* region = builder_.getInsertionBlock()->getParent();
    auto* continuation = new mlir::Block;
    if (resultType != TypeId{0} && resultType != TypeId{5})
      continuation->addArgument(type(resultType), location);
    region->push_back(continuation);
    mlir::Block* nextTest = builder_.getInsertionBlock();
    for (size_t i = 0; i < match.arms.size(); ++i) {
      builder_.setInsertionPointToEnd(nextTest);
      auto* armBlock = new mlir::Block;
      auto* following = i + 1 == match.arms.size() ? nullptr : new mlir::Block;
      region->push_back(armBlock);
      if (following) {
        region->push_back(following);
        mlir::Value condition = patternCondition(*match.arms[i].pattern,
                                                 valueType, value, location);
        builder_.create<mlir::cf::CondBranchOp>(location, condition, armBlock,
                                                mlir::ValueRange{}, following,
                                                mlir::ValueRange{});
      } else {
        builder_.create<mlir::cf::BranchOp>(location, armBlock);
      }
      builder_.setInsertionPointToStart(armBlock);
      environment_.enterScope();
      bindPatternValues(*match.arms[i].pattern, valueType, value, location);
      mlir::Value armValue = emitExpr(*match.arms[i].value);
      if (!terminated()) {
        cleanupScope(location);
        builder_.create<mlir::cf::BranchOp>(
            location, continuation,
            armValue ? mlir::ValueRange{armValue} : mlir::ValueRange{});
      }
      environment_.leaveScope();
      nextTest = following;
    }
    builder_.setInsertionPointToStart(continuation);
    release(value, valueType, location);
    return continuation->getNumArguments() ? continuation->getArgument(0)
                                           : mlir::Value{};
  }

  void emitWhile(const Expr& expression, const WhileExpr& loop) {
    mlir::Location location = loc(expression.location);
    auto* region = builder_.getInsertionBlock()->getParent();
    auto* conditionBlock = new mlir::Block;
    auto* bodyBlock = new mlir::Block;
    auto* continuation = new mlir::Block;
    region->push_back(conditionBlock);
    region->push_back(bodyBlock);
    region->push_back(continuation);
    builder_.create<mlir::cf::BranchOp>(location, conditionBlock);
    builder_.setInsertionPointToStart(conditionBlock);
    mlir::Value condition = emitExpr(*loop.condition);
    builder_.create<mlir::cf::CondBranchOp>(location, condition, bodyBlock,
                                            mlir::ValueRange{}, continuation,
                                            mlir::ValueRange{});
    builder_.setInsertionPointToStart(bodyBlock);
    loopTargets_.push_back(
        LoopTargets{.breakTarget = continuation,
                    .continueTarget = conditionBlock,
                    .continueValue = {},
                    .scopeDepth = environment_.scopeDepth()});
    emitBlock(*loop.body);
    loopTargets_.pop_back();
    if (!terminated())
      builder_.create<mlir::cf::BranchOp>(location, conditionBlock);
    builder_.setInsertionPointToStart(continuation);
  }

  void emitFor(const Expr& expression, const ForExpr& loop) {
    mlir::Location location = loc(expression.location);
    bool rangeLoop = std::holds_alternative<RangeExpr>(loop.iterable->value);
    mlir::Value iterable;
    mlir::Value start;
    mlir::Value end;
    TypeId iterableType = semantics_.expressionTypes.at(loop.iterable->id);
    if (rangeLoop) {
      const auto& range = std::get<RangeExpr>(loop.iterable->value);
      start = emitExpr(*range.start);
      end = emitExpr(*range.end);
    } else {
      iterable = emitExpr(*loop.iterable);
      start = builder_.create<mlir::arith::ConstantIntOp>(location, 0, 64);
      end = runtime(location, "list.length", TypeId{2}, iterable);
    }
    auto* region = builder_.getInsertionBlock()->getParent();
    auto* header = new mlir::Block;
    header->addArgument(builder_.getI64Type(), location);
    auto* body = new mlir::Block;
    body->addArgument(builder_.getI64Type(), location);
    auto* increment = new mlir::Block;
    increment->addArgument(builder_.getI64Type(), location);
    auto* continuation = new mlir::Block;
    region->push_back(header);
    region->push_back(body);
    region->push_back(increment);
    region->push_back(continuation);
    builder_.create<mlir::cf::BranchOp>(location, header, start);
    builder_.setInsertionPointToStart(header);
    mlir::Value index = header->getArgument(0);
    mlir::Value condition = builder_.create<mlir::arith::CmpIOp>(
        location, mlir::arith::CmpIPredicate::slt, index, end);
    builder_.create<mlir::cf::CondBranchOp>(location, condition, body, index,
                                            continuation, mlir::ValueRange{});
    builder_.setInsertionPointToStart(body);
    environment_.enterScope();
    LocalId local = semantics_.loopBindings.at(expression.id);
    TypeId elementType = semantics_.typeTable[iterableType].arguments[0];
    mlir::Value element =
        rangeLoop ? body->getArgument(0)
                  : runtime(location, "list.get.unchecked", elementType,
                            {iterable, body->getArgument(0)});
    bindLocal(local, element, elementType, false, location);
    loopTargets_.push_back(
        LoopTargets{.breakTarget = continuation,
                    .continueTarget = increment,
                    .continueValue = body->getArgument(0),
                    .scopeDepth = environment_.scopeDepth() - 1});
    emitBlock(*loop.body, false);
    loopTargets_.pop_back();
    if (!terminated()) {
      cleanupScope(location);
      builder_.create<mlir::cf::BranchOp>(location, increment,
                                          body->getArgument(0));
    }
    environment_.leaveScope();
    builder_.setInsertionPointToStart(increment);
    auto one = builder_.create<mlir::arith::ConstantIntOp>(location, 1, 64);
    auto next = builder_.create<mlir::arith::AddIOp>(
        location, increment->getArgument(0), one);
    builder_.create<mlir::cf::BranchOp>(location, header,
                                        mlir::ValueRange{next.getResult()});
    builder_.setInsertionPointToStart(continuation);
    if (!rangeLoop)
      release(iterable, iterableType, location);
  }

  void emitLoop(const Expr& expression, const LoopExpr& loop) {
    mlir::Location location = loc(expression.location);
    auto* region = builder_.getInsertionBlock()->getParent();
    auto* body = new mlir::Block;
    auto* continuation = new mlir::Block;
    region->push_back(body);
    region->push_back(continuation);
    builder_.create<mlir::cf::BranchOp>(location, body);
    builder_.setInsertionPointToStart(body);
    loopTargets_.push_back(
        LoopTargets{.breakTarget = continuation,
                    .continueTarget = body,
                    .continueValue = {},
                    .scopeDepth = environment_.scopeDepth()});
    emitBlock(*loop.body);
    loopTargets_.pop_back();
    if (!terminated())
      builder_.create<mlir::cf::BranchOp>(location, body);
    builder_.setInsertionPointToStart(continuation);
  }

  void emitControl(const Expr& expression, const ControlExpr& control) {
    mlir::Location location = loc(expression.location);
    if (control.kind == ControlKind::Return) {
      mlir::Value value;
      if (control.value)
        value = emitExpr(*control.value);
      cleanupAll();
      emitFunctionReturn(location, FunctionReturnStatus{status(0)},
                         FunctionReturnValue{value});
      return;
    }
    const auto& target = loopTargets_.back();
    environment_.cleanupExited(target.scopeDepth,
                               [&](LocalId id) { releaseLocal(id, location); });
    if (control.kind == ControlKind::Break)
      builder_.create<mlir::cf::BranchOp>(location, target.breakTarget);
    else
      builder_.create<mlir::cf::BranchOp>(
          location, target.continueTarget,
          target.continueValue ? mlir::ValueRange{target.continueValue}
                               : mlir::ValueRange{});
  }

  mlir::MLIRContext& context_;
  const std::vector<Module>& modules_;
  const SemanticResult& semantics_;
  mlir::OpBuilder builder_;
  mlir::ModuleOp module_;
  mlir::Value currentContext_;
  TypeId currentResult_{0};
  FunctionRef currentFunction_{.module = ModuleId{0},
                               .function = FunctionIndex{0}};
  std::unordered_map<FunctionRef, mlir::func::FuncOp, FunctionRefHash>
      functions_;
  std::unordered_map<std::uint64_t, std::string> runtimeTypeSpellings_;
  bool typeIdentityCollision_ = false;
  LocalEnvironment environment_;
  std::vector<LoopTargets> loopTargets_;
};

} // namespace

mlir::OwningOpRef<mlir::ModuleOp>
generateMlir(mlir::MLIRContext& context, const std::vector<Module>& modules,
             const SemanticResult& semantics) {
  return Generator(context, modules, semantics).run();
}

} // namespace dola
