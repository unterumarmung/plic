// RUN: not dola-opt %s 2>&1 | FileCheck %s
module {
  func.func @invalid(%integer: i64) {
// CHECK: {{.*}}dialect_invalid_runtime.mlir:[[@LINE+1]]:10: error: 'dola.runtime' op has unknown runtime callee `unknown`
    %0 = "dola.runtime"(%integer) <{callee = "unknown", metadata = array<i32>, typeId = 0 : i64}> : (i64) -> i64
    return
  }
}
