# Negative coverage inventory

This inventory maps deterministic rejection and status families to their
regression tests. Diagnostic fixtures keep FileCheck directives adjacent to the
source construct and use `[[@LINE+N]]` relative locations. Cross-file ordering
is asserted through structured `Diagnostic` locations in `sema_test.cpp`.

## Lexer and parser

| Rejection family | Tests |
| --- | --- |
| Illegal characters and exact token locations | `compiler/lexer/illegal_character.dola`, `compiler/lexer/lexer_test.cpp` |
| Invalid escapes, unterminated strings, unterminated comments, comments and whitespace | `compiler/lexer/malformed_string.dola`, `compiler/lexer/unterminated_comment.dola`, `compiler/lexer/lexer_test.cpp` |
| `loop` and other capability-keyword token locations | `compiler/lexer/lexer_test.cpp` |
| Required module declaration and malformed declarations/types | `compiler/syntax/missing_module.dola`, `compiler/syntax/malformed_type.dola`, `compiler/syntax/syntax_test.cpp` |
| Malformed generic constructors and postfix chains | `compiler/syntax/malformed_constructor.dola`, `compiler/syntax/invalid_postfix.dola` |
| Condition-bearing and missing-block unconditional loops | `compiler/syntax/malformed_loop.dola` |
| Integer, float, integer-pattern, and tuple-index overflow | `compiler/syntax/literal_overflow.dola`, `compiler/syntax/syntax_test.cpp` |
| Missing delimiters, malformed parameters/statements, parser recovery, and multiple syntax errors | `compiler/syntax/syntax_test.cpp` |
| Stable expression/declaration/pattern identities after rejected and repeated parses | `compiler/syntax/syntax_test.cpp`, `compiler/support/strong_id_test.cpp` |

## Module graph and semantic analysis

| Rejection family | Tests |
| --- | --- |
| Duplicate modules, import aliases, declarations across kinds, fields, variants, parameters, locals, and pattern bindings | `compiler/sema/declaration_diagnostics.dola`, `compiler/sema/diagnostics.dola`, `compiler/sema/sema_test.cpp` |
| Missing/cyclic imports, invalid qualifiers, visibility, unresolved names, declaration-before-use, and source ordering | `compiler/sema/diagnostics.dola`, `compiler/sema/diagnostic_ordering*.dola`, `compiler/sema/sema_test.cpp` |
| Recursive named types and unknown/private named types | `compiler/sema/aggregate_diagnostics.dola`, `compiler/sema/sema_test.cpp` |
| Tuple arity, destructuring, mutability, member bounds, assignment, return, and body-result typing | `compiler/sema/aggregate_diagnostics.dola`, `compiler/sema/type_operator_diagnostics.dola` |
| Unary, arithmetic, comparison, equality, logical, condition, branch, and range type matrices | `compiler/sema/type_operator_diagnostics.dola`, `compiler/sema/sema_test.cpp` |
| Loop iterable/condition typing and invalid `break`/`continue` contexts | `compiler/sema/type_operator_diagnostics.dola`, `compiler/sema/control_result_diagnostics.dola` |
| Non-`Unit` loop bodies, mixed string concatenation, unsupported string arithmetic, `Int.to_string` receiver/arity, and invalid panic calls | `compiler/sema/text_loop_diagnostics.dola` |
| Function, output, and built-in method receiver/arity/argument failures | `compiler/sema/call_collection_diagnostics.dola` |
| Record construction/update completeness, duplicates, wrong fields, wrong declaration kinds, and field types | `compiler/sema/aggregate_diagnostics.dola`, `compiler/sema/aggregate_pattern_diagnostics.dola` |
| Enum, `Option`, `Result`, and `IndexError` constructor inference, arity, payload, and postfix `?` | `compiler/sema/call_collection_diagnostics.dola`, `compiler/sema/control_result_diagnostics.dola`, `compiler/sema/aggregate_pattern_diagnostics.dola` |
| Empty/heterogeneous lists, map key restrictions, collection constructors, and collection methods | `compiler/sema/call_collection_diagnostics.dola`, `compiler/sema/sema_test.cpp` |
| Pattern subject/payload/binding/literal errors, exhaustiveness, duplicate and unreachable arms | `compiler/sema/aggregate_pattern_diagnostics.dola`, `compiler/sema/sema_test.cpp` |
| Missing, duplicate, parameterized, and invalid-result executable entry points | `compiler/sema/diagnostics.dola`, `compiler/driver/driver_errors.dola`, `compiler/sema/sema_test.cpp` |
| Invalid spawn forms, channel type/argument mismatches, resource methods, missing built-in imports, and non-serializable transport messages | `compiler/sema/concurrency_transport_diagnostics.dola`, `compiler/sema/sema_test.cpp` |

## Driver, MLIR, and lowering

| Rejection family | Tests |
| --- | --- |
| Missing sources, unknown/repeated actions, missing `-o`, unreadable inputs, phase failure, and invalid object paths | `compiler/driver/driver_errors.dola`, `compiler/driver/compiler_driver_test.cpp`, `compiler/mlir/mlir_api_test.cpp` |
| Invalid Dola output operands and tuple arity | `compiler/mlir/dialect_invalid.mlir`, `compiler/mlir/dialect_invalid_types.mlir` |
| Equality and retain/release type consistency | `compiler/mlir/dialect_invalid_operations.mlir`, `compiler/mlir/dialect_invalid_retain*.mlir`, `compiler/mlir/dialect_invalid_release.mlir` |
| Slot allocation/load/store consistency | `compiler/mlir/dialect_invalid_slot_*.mlir` |
| Unknown runtime operations | `compiler/mlir/dialect_invalid_runtime.mlir`, `runtime/runtime_unknown_operation_test` |
| Runtime registry input/result/metadata arity, text-operation relationships, fixed opcode values, and recursive nested verification | `compiler/mlir/dialect_invalid_text_runtime.mlir`, `compiler/mlir/mlir_api_test.cpp` |
| Illegal operations surviving conversion, non-LLVM translation inputs, and object-output creation failure | `compiler/mlir/mlir_api_test.cpp` |
| Division/remainder zero and signed division overflow guards | `compiler/lowering/division.dola`, `integration/division_by_zero.dola`, `integration/remainder_by_zero.dola`, `integration/division_overflow.dola` |
| Context threading, string runtime dispatch, poison elimination, and panic lowering | `compiler/lowering/text_panic.mlir`, `compiler/mlir/dialect.mlir` |
| Undeclared versus declared transitive Bazel inputs | `integration/undeclared_import_compile_test`, `integration/declared_import_compile_test` |

## Runtime ABI and generated executables

| Rejection/status family | Tests |
| --- | --- |
| ABI version, runtime create/destroy, null runtime output, and last-error replacement | `runtime/runtime_abi_test.cpp`, `runtime/runtime_test.rs` |
| String concatenation and integer formatting null outputs, wrong kinds, output preservation, UTF-8, large values, and integer boundaries | `runtime/runtime_abi_test.cpp`, `integration/string_behavior.dola` |
| Invalid value tags, null object values, equality output preservation, and total retain/release cleanup | `runtime/runtime_abi_test.cpp` |
| Byte/string nulls, invalid UTF-8, wrong kinds, null scalar/value outputs, and print variants | `runtime/runtime_abi_test.cpp`, `runtime/runtime_test.rs` |
| Tuple/record/enum null inputs/outputs, cross-kind calls, bounds, immutable updates, and ownership balance | `runtime/runtime_abi_test.cpp` |
| List nulls, cross-kind calls, negative/out-of-range access/update/removal, negative `take_last`, and source preservation | `runtime/runtime_abi_test.cpp`, `integration/negative_take_last.dola` |
| Map key-kind validation, wrong keys/kinds, null outputs, missing values, entries, equality, and source preservation | `runtime/runtime_abi_test.cpp`, `runtime/runtime_test.rs` |
| Codegen null output, missing input, unknown opcode, equality/printing null pointers, and retain/release null pointers | `runtime/runtime_abi_test.cpp`, `runtime/runtime_*_test` |
| Malformed `Result` entry values and failing `Result` mains | `runtime/runtime_malformed_result_test`, `integration/result_main_failure.dola` |
| Live-object balance and bounded concurrent retain/read/update | `runtime/runtime_abi_test.cpp`, `runtime/runtime_test.rs` |
| Channel wrong kinds, close/send/receive transitions, task context validation, repeated joins, and contained task failures | `runtime/runtime_abi_test.cpp`, `integration/concurrency_execution_test` |
| Main, nested, repeated-join, division-in-task, and sibling-survival panic behavior | `integration/main_panic.dola`, `integration/task_panic.dola`, `runtime/runtime_abi_test.cpp` |
| Text I/O failures, integer parsing, negative sleep, transport wrong kinds, handshake mismatch, malformed/oversized frame validation, full-duplex blocking I/O, peer closure, and ephemeral-port communication | `runtime/runtime_abi_test.cpp`, `runtime/runtime_test.rs`, `integration/typed_transport_execution_test`, `integration/chat_execution_test.py` |
| Chat invalid/duplicate names, malformed and missing-recipient DMs, message limits, privacy, bounded history, abrupt disconnect, and 64-client churn | `integration/chat_execution_test.py`, `integration/chat_stress_test.py` |

## Narrow exclusions

The suite intentionally does not manufacture undefined behavior. In
particular, it does not pass arbitrary invalid non-null pointers across the C
ABI, force allocator failure, corrupt `Arc` bookkeeping, or force target-machine
failures for unsupported hosts. Those paths cannot be induced safely and
deterministically without adding public fault-injection ABI or relying on host
behavior, both of which are outside this test contract.
