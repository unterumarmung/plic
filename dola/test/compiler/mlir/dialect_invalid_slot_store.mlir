// RUN: not dola-opt %s 2>&1 | FileCheck %s
module {
  func.func @invalid(%float: f64, %slot: !dola.slot<i64>) {
// CHECK: {{.*}}dialect_invalid_slot_store.mlir:[[@LINE+1]]:5: error: 'dola.slot.store' op requires the value to match the slot type
    "dola.slot.store"(%float, %slot) : (f64, !dola.slot<i64>) -> ()
    return
  }
}
