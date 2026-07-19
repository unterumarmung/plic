include "mlir/IR/DialectBase.td"
include "mlir/IR/OpBase.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

def Dola_Dialect : Dialect {
  let name = "dola";
  let cppNamespace = "::dola::ir";
  let summary = "High-level operations for checked Dola semantics";
  let useDefaultTypePrinterParser = 1;
}

class Dola_Type<string name, string mnemonicName> : TypeDef<Dola_Dialect, name> {
  let mnemonic = mnemonicName;
}

def Dola_StringType : Dola_Type<"String", "string"> {
  let summary = "immutable reference-counted UTF-8 string";
}
def Dola_UnitType : Dola_Type<"Unit", "unit">;
def Dola_ContextType : Dola_Type<"Context", "context"> {
  let summary = "hidden task-local execution context";
}

def Dola_RecordType : Dola_Type<"Record", "record"> {
  let summary = "named record value";
  let parameters = (ins StringRefParameter<"declaration identity">:$identity);
  let assemblyFormat = "`<` $identity `>`";
}

def Dola_EnumType : Dola_Type<"Enum", "enum"> {
  let summary = "named enum value";
  let parameters = (ins StringRefParameter<"declaration identity">:$identity);
  let assemblyFormat = "`<` $identity `>`";
}

def Dola_TupleType : Dola_Type<"Tuple", "tuple"> {
  let summary = "tuple value";
  let parameters = (ins ArrayRefParameter<"::mlir::Type">:$elementTypes);
  let assemblyFormat = "`<` $elementTypes `>`";
  let genVerifyDecl = 1;
}

class Dola_UnaryType<string name, string mnemonicName>
    : Dola_Type<name, mnemonicName> {
  let parameters = (ins "::mlir::Type":$elementType);
  let assemblyFormat = "`<` $elementType `>`";
}

def Dola_OptionType : Dola_UnaryType<"Option", "option">;
def Dola_ListType : Dola_UnaryType<"List", "list">;
def Dola_SlotType : Dola_UnaryType<"Slot", "slot">;
def Dola_SenderType : Dola_UnaryType<"Sender", "sender">;
def Dola_ReceiverType : Dola_UnaryType<"Receiver", "receiver">;
def Dola_TaskType : Dola_UnaryType<"Task", "task">;

class Dola_BinaryType<string name, string mnemonicName>
    : Dola_Type<name, mnemonicName> {
  let parameters = (ins "::mlir::Type":$firstType, "::mlir::Type":$secondType);
  let assemblyFormat = "`<` $firstType `,` $secondType `>`";
}

def Dola_ResultType : Dola_BinaryType<"Result", "result">;
def Dola_MapType : Dola_BinaryType<"Map", "map">;
def Dola_ListenerType : Dola_BinaryType<"Listener", "listener">;
def Dola_ConnectionType : Dola_BinaryType<"Connection", "connection">;

class Dola_Op<string mnemonic, list<Trait> traits = []>
    : Op<Dola_Dialect, mnemonic, traits>;

def Dola_TaskSpawnOp : Dola_Op<"task.spawn"> {
  let arguments = (ins Dola_ContextType:$context, FlatSymbolRefAttr:$callee,
                       Variadic<AnyType>:$inputs);
  let results = (outs Dola_TaskType:$result);
  let assemblyFormat = "$context `,` $callee `(` $inputs `)` attr-dict `:` functional-type($inputs, results)";
  let hasVerifier = 1;
}

def Dola_StringLiteralOp : Dola_Op<"string.literal", [Pure]> {
  let arguments = (ins StrAttr:$value);
  let results = (outs Dola_StringType:$result);
  let assemblyFormat = "$value attr-dict `:` type($result)";
}

def Dola_PrintOp : Dola_Op<"io.print"> {
  let arguments = (ins Dola_StringType:$value);
  let assemblyFormat = "$value attr-dict `:` type($value)";
}

def Dola_PrintlnOp : Dola_Op<"io.println"> {
  let arguments = (ins Dola_StringType:$value);
  let assemblyFormat = "$value attr-dict `:` type($value)";
}

def Dola_PanicOp : Dola_Op<"panic"> {
  let arguments = (ins Dola_ContextType:$context, Dola_StringType:$message);
  let assemblyFormat = "$context `,` $message attr-dict `:` type($message)";
}

def Dola_PoisonOp : Dola_Op<"poison", [Pure]> {
  let results = (outs AnyType:$result);
  let assemblyFormat = "attr-dict `:` type($result)";
}

def Dola_RuntimeOp : Dola_Op<"runtime", []> {
  let summary = "typed operation lowered to one runtime ABI family";
  let arguments = (ins StrAttr:$callee, I64Attr:$typeId,
                       DenseI32ArrayAttr:$metadata, Variadic<AnyType>:$inputs);
  let results = (outs Variadic<AnyType>:$outputs);
  let hasVerifier = 1;
}

def Dola_ValueEqualOp : Dola_Op<"value.equal", [Pure]> {
  let arguments = (ins AnyType:$left, AnyType:$right);
  let results = (outs I1:$result);
  let assemblyFormat = "$left `,` $right attr-dict `:` type($left) `,` type($right)";
  let hasVerifier = 1;
}

def Dola_RetainOp : Dola_Op<"value.retain"> {
  let arguments = (ins AnyType:$value);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$value attr-dict `:` type($value) `->` type($result)";
  let hasVerifier = 1;
}

def Dola_ReleaseOp : Dola_Op<"value.release"> {
  let arguments = (ins AnyType:$value);
  let assemblyFormat = "$value attr-dict `:` type($value)";
  let hasVerifier = 1;
}

def Dola_SlotAllocOp : Dola_Op<"slot.alloc"> {
  let arguments = (ins AnyType:$initial);
  let results = (outs Dola_SlotType:$slot);
  let assemblyFormat = "$initial attr-dict `:` type($initial) `->` type($slot)";
  let hasVerifier = 1;
}

def Dola_SlotLoadOp : Dola_Op<"slot.load"> {
  let arguments = (ins Dola_SlotType:$slot);
  let results = (outs AnyType:$value);
  let assemblyFormat = "$slot attr-dict `:` type($slot) `->` type($value)";
  let hasVerifier = 1;
}

def Dola_SlotStoreOp : Dola_Op<"slot.store"> {
  let arguments = (ins AnyType:$value, Dola_SlotType:$slot);
  let assemblyFormat = "$value `,` $slot attr-dict `:` type($value) `,` type($slot)";
  let hasVerifier = 1;
}
