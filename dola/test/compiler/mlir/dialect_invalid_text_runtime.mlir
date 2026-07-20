// RUN: not dola-opt --split-input-file %s 2>&1 | FileCheck %s
module {
  func.func @invalid_input(%text: !dola.string) {
// CHECK: {{.*}}dialect_invalid_text_runtime.mlir:[[@LINE+1]]:10: error: 'dola.runtime' op has an invalid input count for `string.concat`
    %0 = "dola.runtime"(%text) <{callee = "string.concat", metadata = array<i32>, typeId = 0 : i64}> : (!dola.string) -> !dola.string
    return
  }
}

// -----

module {
  func.func @invalid_result(%text: !dola.string) {
// CHECK: {{.*}}dialect_invalid_text_runtime.mlir:[[@LINE+1]]:5: error: 'dola.runtime' op `string.concat` requires 1 results but has 0
    "dola.runtime"(%text, %text) <{callee = "string.concat", metadata = array<i32>, typeId = 0 : i64}> : (!dola.string, !dola.string) -> ()
    return
  }
}

// -----

module {
  func.func @invalid_metadata(%text: !dola.string) {
// CHECK: {{.*}}dialect_invalid_text_runtime.mlir:[[@LINE+1]]:10: error: 'dola.runtime' op requires 0 metadata values
    %0 = "dola.runtime"(%text, %text) <{callee = "string.concat", metadata = array<i32: 1>, typeId = 0 : i64}> : (!dola.string, !dola.string) -> !dola.string
    return
  }
}

// -----

module {
  func.func @invalid_relationship(%text: !dola.string) {
// CHECK: {{.*}}dialect_invalid_text_runtime.mlir:[[@LINE+1]]:10: error: 'dola.runtime' op `int.to_string` requires an Int input and String result
    %0 = "dola.runtime"(%text) <{callee = "int.to_string", metadata = array<i32>, typeId = 0 : i64}> : (!dola.string) -> !dola.string
    return
  }
}
