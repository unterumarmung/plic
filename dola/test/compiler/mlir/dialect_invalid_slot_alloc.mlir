// RUN: not dola-opt %s 2>&1 | FileCheck %s
module {
  func.func @invalid(%integer: i64) {
// CHECK: {{.*}}dialect_invalid_slot_alloc.mlir:[[@LINE+1]]:10: error: 'dola.slot.alloc' op requires the initial value to match the slot type
    %0 = "dola.slot.alloc"(%integer) : (i64) -> !dola.slot<f64>
    return
  }
}
