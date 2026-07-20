// RUN: not dola-opt %s 2>&1 | FileCheck %s
module {
  func.func @invalid(%string: !dola.string) {
// CHECK: {{.*}}dialect_invalid_retain.mlir:[[@LINE+1]]:10: error: 'dola.value.retain' op requires matching input and result types
    %0 = "dola.value.retain"(%string) : (!dola.string) -> i64
    return
  }
}
