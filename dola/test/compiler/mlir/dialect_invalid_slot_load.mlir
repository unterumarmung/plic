// RUN: not dola-opt %s 2>&1 | FileCheck %s
module {
  func.func @invalid(%slot: !dola.slot<i64>) {
// CHECK: {{.*}}dialect_invalid_slot_load.mlir:[[@LINE+1]]:10: error: 'dola.slot.load' op requires the result to match the slot type
    %0 = "dola.slot.load"(%slot) : (!dola.slot<i64>) -> f64
    return
  }
}
