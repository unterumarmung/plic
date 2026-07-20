// RUN: dola-opt --dola-lower-to-llvm %s | FileCheck %s
module {
  func.func @text(%context: !dola.context, %left: !dola.string,
                  %right: !dola.string) -> !dola.string {
    %result = "dola.runtime"(%left, %right) <{callee = "string.concat",
      metadata = array<i32>, typeId = 0 : i64}> :
      (!dola.string, !dola.string) -> !dola.string
    return %result : !dola.string
  }
  func.func @fail(%context: !dola.context, %message: !dola.string) -> i32 {
    dola.panic %context, %message : !dola.string
    %status = arith.constant 5 : i32
    return %status : i32
  }
}

// CHECK: llvm.func @text
// CHECK: llvm.mlir.constant(46 : i32)
// CHECK: llvm.call @dola_rt_codegen
// CHECK: llvm.func @fail
// CHECK: llvm.call @dola_rt_codegen_context_set_panic
// CHECK-NOT: dola.
