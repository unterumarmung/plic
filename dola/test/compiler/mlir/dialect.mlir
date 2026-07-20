// RUN: dola-opt %s | FileCheck %s
// CHECK: func.func @write(%{{.*}}: !dola.string)
// CHECK: dola.io.println %{{.*}} : !dola.string
// CHECK: func.func @types(%{{.*}}: !dola.record<"example.Point">, %{{.*}}: !dola.enum<"example.Value">, %{{.*}}: !dola.tuple<i64, !dola.string>, %{{.*}}: !dola.option<i64>, %{{.*}}: !dola.result<i64, !dola.string>, %{{.*}}: !dola.list<i64>, %{{.*}}: !dola.map<!dola.string, i64>)
// CHECK: func.func @failure_values(%{{.*}}: !dola.context, %{{.*}}: !dola.string) -> i64
// CHECK: dola.panic %{{.*}}, %{{.*}} : !dola.string
// CHECK: dola.poison : i64
module {
  func.func @write(%text: !dola.string) {
    dola.io.println %text : !dola.string
    return
  }
  func.func @types(%record: !dola.record<"example.Point">,
                   %enum: !dola.enum<"example.Value">,
                   %tuple: !dola.tuple<i64, !dola.string>,
                   %option: !dola.option<i64>,
                   %result: !dola.result<i64, !dola.string>,
                   %list: !dola.list<i64>,
                   %map: !dola.map<!dola.string, i64>) {
    return
  }
  func.func @failure_values(%context: !dola.context,
                            %message: !dola.string) -> i64 {
    dola.panic %context, %message : !dola.string
    %poison = dola.poison : i64
    return %poison : i64
  }
}
