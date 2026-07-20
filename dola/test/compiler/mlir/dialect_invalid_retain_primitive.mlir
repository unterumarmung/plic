// RUN: not dola-opt %s 2>&1 | FileCheck %s
module {
  func.func @invalid(%integer: i64) {
// CHECK: {{.*}}dialect_invalid_retain_primitive.mlir:[[@LINE+1]]:10: error: 'dola.value.retain' op requires a runtime-backed value
    %0 = "dola.value.retain"(%integer) : (i64) -> i64
    return
  }
}
