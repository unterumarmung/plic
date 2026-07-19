#ifndef DOLA_SEMA_SEMA_H
#define DOLA_SEMA_SEMA_H

#include "dola/ast.h"
#include "dola/diagnostic.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dola {

struct TypeIdTag;
struct LocalIdTag;
struct FieldIndexTag;
struct VariantIndexTag;
struct RuntimeTypeIdTag;

using TypeId = StrongId<TypeIdTag>;
using LocalId = StrongId<LocalIdTag>;
using FieldIndex = StrongId<FieldIndexTag>;
using VariantIndex = StrongId<VariantIndexTag>;
using RuntimeTypeId = StrongId<RuntimeTypeIdTag, std::uint64_t>;

struct FunctionRef {
  ModuleId module;
  FunctionIndex function;
  friend bool operator==(const FunctionRef&, const FunctionRef&) = default;
};
struct RecordRef {
  ModuleId module;
  RecordIndex record;
  friend bool operator==(const RecordRef&, const RecordRef&) = default;
};
struct EnumRef {
  ModuleId module;
  EnumIndex enumeration;
  friend bool operator==(const EnumRef&, const EnumRef&) = default;
};
using TypeDeclarationRef = std::variant<RecordRef, EnumRef>;

enum class TypeKind : std::uint8_t {
  Unit,
  Bool,
  Int,
  Float,
  String,
  Record,
  Enum,
  Tuple,
  Option,
  Result,
  List,
  Map,
  Sender,
  Receiver,
  Task,
  Listener,
  Connection,
  Range,
  IndexError,
  ChannelError,
  TaskError,
  IoError,
  TransportError,
  ParseIntError,
  Function,
  Never,
  Error,
};

struct SemanticType {
  TypeKind kind;
  std::vector<TypeId> arguments;
  std::optional<TypeDeclarationRef> declaration;
};

struct TypeTable {
  std::vector<SemanticType> types;

  const SemanticType& operator[](TypeId id) const { return at(types, id); }
  std::string name(TypeId id, const std::vector<Module>& modules) const;
  bool runtimeBacked(TypeId id) const;
  bool supportsEquality(TypeId id) const;
};

enum class BuiltinMethod : std::uint8_t {
  StringLength,
  IntToString,
  ListLength,
  ListIsEmpty,
  ListGet,
  ListPush,
  ListSet,
  ListRemoveAt,
  ListTakeLast,
  MapLength,
  MapIsEmpty,
  MapContains,
  MapGet,
  MapInsert,
  MapRemove,
  MapEntries,
  StringSplitOnce,
  SenderSend,
  SenderClose,
  ReceiverReceive,
  TaskJoin,
  ListenerAccept,
  ListenerLocalAddress,
  ListenerClose,
  ConnectionSend,
  ConnectionReceive,
  ConnectionClose,
};

enum class BuiltinFunction : std::uint8_t {
  Print,
  Println,
  ReadLine,
  ReadText,
  WriteText,
  UnixSeconds,
  SleepMilliseconds,
  ParseInt,
  Panic,
  ChannelCreate,
  TransportListen,
  TransportConnect,
};

struct LocalReference {
  LocalId local;
};
struct FunctionReference {
  FunctionRef function;
};
struct OutputReference {
  bool newline;
};
struct RecordFieldReference {
  RecordRef record;
  FieldIndex field;
};
struct RecordConstructorReference {
  RecordRef record;
};
struct TupleFieldReference {
  TupleElementIndex field;
};
struct EnumVariantReference {
  EnumRef enumeration;
  VariantIndex variant;
};
struct OptionConstructorReference {
  bool some;
};
struct ResultConstructorReference {
  bool ok;
};
struct BuiltinMethodReference {
  BuiltinMethod method;
};
struct BuiltinFunctionReference {
  BuiltinFunction function;
};
struct SpawnReference {
  FunctionRef function;
};
struct IndexErrorConstructorReference {};
enum class StandardError : std::uint8_t {
  Index,
  Channel,
  Task,
  Io,
  Transport,
  ParseInt,
};
struct StandardErrorConstructorReference {
  StandardError error;
  VariantIndex variant;
};
using ResolvedReference = std::variant<
    LocalReference, FunctionReference, OutputReference, RecordFieldReference,
    RecordConstructorReference, TupleFieldReference, EnumVariantReference,
    OptionConstructorReference, ResultConstructorReference,
    BuiltinMethodReference, BuiltinFunctionReference, SpawnReference,
    IndexErrorConstructorReference, StandardErrorConstructorReference>;

struct FunctionTypeInfo {
  std::vector<TypeId> parameters;
  TypeId result{0};
};
struct RecordTypeInfo {
  TypeId type{0};
  std::vector<TypeId> fields;
};
struct EnumTypeInfo {
  TypeId type{0};
  std::vector<std::vector<TypeId>> variants;
};

struct SemanticResult {
  TypeTable typeTable;
  std::unordered_map<ExprId, TypeId> expressionTypes;
  std::unordered_map<const TypeRef*, TypeId> resolvedTypes;
  std::unordered_map<ExprId, ResolvedReference> references;
  std::unordered_map<const Statement*, std::vector<LocalId>> bindings;
  std::unordered_map<const Statement*, LocalId> assignments;
  std::unordered_map<PatternId, ResolvedReference> patterns;
  std::unordered_map<PatternId, LocalId> patternBindings;
  std::unordered_map<ExprId, LocalId> loopBindings;
  std::unordered_map<DeclId, FunctionTypeInfo> functions;
  std::unordered_map<DeclId, RecordTypeInfo> records;
  std::unordered_map<DeclId, EnumTypeInfo> enums;
  std::optional<FunctionRef> entryPoint;
};

std::optional<SemanticResult> analyze(const std::vector<Module>& modules,
                                      DiagnosticEngine& diagnostics,
                                      bool requireEntryPoint);

} // namespace dola
#endif
