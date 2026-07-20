// RUN: not dola-opt %s 2>&1 | FileCheck %s
module {
  func.func @invalid(%integer: i64, %float: f64) {
// CHECK: {{.*}}dialect_invalid_operations.mlir:[[@LINE+1]]:10: error: 'dola.value.equal' op requires operands with the same type
    %0 = "dola.value.equal"(%integer, %float) : (i64, f64) -> i1
    return
  }
}
