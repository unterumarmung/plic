// RUN: not dola-opt %s 2>&1 | FileCheck %s
module {
// CHECK: {{.*}}dialect_invalid_types.mlir:[[@LINE+1]]:43: error: tuple type requires 2 to 8 elements
  func.func @too_small(%value: !dola.tuple<i64>) {
    return
  }
}
