#ifndef DOLA_SYNTAX_AST_H
#define DOLA_SYNTAX_AST_H

#include "dola/diagnostic.h"
#include "dola/strong_id.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dola {

struct ExprIdTag;
struct DeclIdTag;
struct PatternIdTag;
struct FunctionIndexTag;
struct RecordIndexTag;
struct EnumIndexTag;
struct TupleElementIndexTag;
struct ModuleIdTag;

using ExprId = StrongId<ExprIdTag, std::uint64_t>;
using DeclId = StrongId<DeclIdTag, std::uint64_t>;
using PatternId = StrongId<PatternIdTag, std::uint64_t>;
using FunctionIndex = StrongId<FunctionIndexTag>;
using RecordIndex = StrongId<RecordIndexTag>;
using EnumIndex = StrongId<EnumIndexTag>;
using TupleElementIndex = StrongId<TupleElementIndexTag>;
using ModuleId = StrongId<ModuleIdTag>;

enum class PrimitiveType : std::uint8_t { Unit, Bool, Int, Float, String };
enum class BuiltinType : std::uint8_t {
  Option,
  Result,
  List,
  Map,
  Sender,
  Receiver,
  Task,
  Listener,
  Connection,
};

struct TypeRef;
using TypeRefPtr = std::unique_ptr<TypeRef>;

struct PrimitiveTypeRef {
  PrimitiveType kind;
};
struct NamedTypeRef {
  std::vector<std::string> components;
};
struct TupleTypeRef {
  std::vector<TypeRefPtr> elements;
};
struct BuiltinTypeRef {
  BuiltinType kind;
  std::vector<TypeRefPtr> arguments;
};
struct TypeRef {
  SourceLocation location;
  std::variant<PrimitiveTypeRef, NamedTypeRef, TupleTypeRef, BuiltinTypeRef>
      value;
};

struct Expr;
struct Block;
struct Pattern;
using ExprPtr = std::unique_ptr<Expr>;
using PatternPtr = std::unique_ptr<Pattern>;

struct LiteralExpr {
  std::variant<std::monostate, bool, int64_t, double, std::string> value;
};
struct NameExpr {
  std::vector<std::string> components;
};
struct CallExpr {
  ExprPtr callee;
  std::vector<TypeRef> typeArguments;
  std::vector<ExprPtr> arguments;
};
struct SpawnExpr {
  ExprPtr call;
};
struct MemberExpr {
  ExprPtr base;
  std::variant<std::string, TupleElementIndex> member;
};
struct UnaryExpr {
  std::string op;
  ExprPtr operand;
};
struct BinaryExpr {
  std::string op;
  ExprPtr left;
  ExprPtr right;
};
struct IfExpr {
  ExprPtr condition;
  std::unique_ptr<Block> thenBlock;
  std::variant<std::monostate, std::unique_ptr<Block>, ExprPtr> elseBranch;
};
struct BlockExpr {
  std::unique_ptr<Block> block;
};
struct TupleExpr {
  std::vector<ExprPtr> elements;
};
struct ListExpr {
  std::vector<ExprPtr> elements;
};
struct GenericConstructorExpr {
  BuiltinType kind;
  std::vector<TypeRef> arguments;
};
struct RecordFieldValue {
  SourceLocation location;
  std::string name;
  ExprPtr value;
};
struct RecordExpr {
  std::vector<std::string> typeName;
  std::vector<RecordFieldValue> fields;
};
struct RecordUpdateExpr {
  ExprPtr base;
  std::vector<RecordFieldValue> fields;
};
struct TryExpr {
  ExprPtr value;
};
struct RangeExpr {
  ExprPtr start;
  ExprPtr end;
};

struct WildcardPattern {};
struct BindingPattern {
  std::string name;
};
struct LiteralPattern {
  std::variant<bool, int64_t, std::string> value;
};
struct ConstructorPattern {
  std::vector<std::string> name;
  std::vector<PatternPtr> arguments;
};
struct Pattern {
  PatternId id;
  SourceLocation location;
  std::variant<WildcardPattern, BindingPattern, LiteralPattern,
               ConstructorPattern>
      value;
};
struct MatchArm {
  SourceLocation location;
  PatternPtr pattern;
  ExprPtr value;
};
struct MatchExpr {
  ExprPtr value;
  std::vector<MatchArm> arms;
};
struct WhileExpr {
  ExprPtr condition;
  std::unique_ptr<Block> body;
};
struct ForExpr {
  std::string binding;
  ExprPtr iterable;
  std::unique_ptr<Block> body;
};
struct LoopExpr {
  std::unique_ptr<Block> body;
};
enum class ControlKind : std::uint8_t { Return, Break, Continue };
struct ControlExpr {
  ControlKind kind;
  ExprPtr value;
};

struct Expr {
  ExprId id;
  SourceLocation location;
  std::variant<LiteralExpr, NameExpr, CallExpr, SpawnExpr, MemberExpr,
               UnaryExpr, BinaryExpr, IfExpr, BlockExpr, TupleExpr, ListExpr,
               GenericConstructorExpr, RecordExpr, RecordUpdateExpr, TryExpr,
               RangeExpr, MatchExpr, WhileExpr, ForExpr, LoopExpr,
               ControlExpr>
      value;
};

struct BindingName {
  std::string name;
};
struct TupleBinding {
  std::vector<std::string> names;
};
using BindingTarget = std::variant<BindingName, TupleBinding>;

struct BindingStmt {
  bool mutableBinding;
  BindingTarget target;
  std::optional<TypeRef> annotation;
  ExprPtr initializer;
};
struct AssignmentStmt {
  std::string name;
  ExprPtr value;
};
struct ExpressionStmt {
  ExprPtr value;
};
struct Statement {
  SourceLocation location;
  std::variant<BindingStmt, AssignmentStmt, ExpressionStmt> value;
};
struct Block {
  SourceLocation location;
  std::vector<Statement> statements;
  ExprPtr tail;
};
struct Parameter {
  SourceLocation location;
  std::string name;
  TypeRef type;
};
struct FunctionDecl {
  DeclId id;
  SourceLocation location;
  bool isPublic;
  std::string name;
  std::vector<Parameter> parameters;
  TypeRef returnType;
  std::unique_ptr<Block> body;
};
struct RecordFieldDecl {
  SourceLocation location;
  std::string name;
  TypeRef type;
};
struct RecordDecl {
  DeclId id;
  SourceLocation location;
  bool isPublic;
  std::string name;
  std::vector<RecordFieldDecl> fields;
};
struct EnumVariantDecl {
  SourceLocation location;
  std::string name;
  std::vector<TypeRef> payloads;
};
struct EnumDecl {
  DeclId id;
  SourceLocation location;
  bool isPublic;
  std::string name;
  std::vector<EnumVariantDecl> variants;
};
struct ImportDecl {
  SourceLocation location;
  std::vector<std::string> moduleName;
};
struct DeclarationOrder {
  std::variant<FunctionIndex, RecordIndex, EnumIndex> index;
};
struct Module {
  std::filesystem::path path;
  SourceLocation location;
  std::vector<std::string> name;
  std::vector<ImportDecl> imports;
  std::vector<FunctionDecl> functions;
  std::vector<RecordDecl> records;
  std::vector<EnumDecl> enums;
  std::vector<DeclarationOrder> declarations;
};

std::string qualifiedName(const std::vector<std::string>& components);
std::string primitiveTypeName(PrimitiveType type);
std::string builtinTypeName(BuiltinType type);
std::string dumpAst(const Module& module);

} // namespace dola
#endif
