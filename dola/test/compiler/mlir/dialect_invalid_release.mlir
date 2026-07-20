// RUN: not dola-opt %s 2>&1 | FileCheck %s
module {
  func.func @invalid(%integer: i64) {
// CHECK: {{.*}}dialect_invalid_release.mlir:[[@LINE+1]]:5: error: 'dola.value.release' op requires a runtime-backed value
    "dola.value.release"(%integer) : (i64) -> ()
    return
  }
}
