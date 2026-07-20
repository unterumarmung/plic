// RUN: not dola-opt %s 2>&1 | FileCheck %s
module {
  func.func @bad(%value: i64) {
// CHECK: {{.*}}dialect_invalid.mlir:[[@LINE+1]]:5: error: 'dola.io.println' op operand #0 must be immutable reference-counted UTF-8 string, but got 'i64'
    "dola.io.println"(%value) : (i64) -> ()
    return
  }
}
