module {
  func.func @strict64_add(%a: i64, %b: i64) -> i64 {
    %x = arith.bitcast %a : i64 to f64
    %y = arith.bitcast %b : i64 to f64
    %r = arith.addf %x, %y : f64
    %bits = arith.bitcast %r : f64 to i64
    return %bits : i64
  }
  func.func @strict64_mul(%a: i64, %b: i64) -> i64 {
    %x = arith.bitcast %a : i64 to f64
    %y = arith.bitcast %b : i64 to f64
    %r = arith.mulf %x, %y : f64
    %bits = arith.bitcast %r : f64 to i64
    return %bits : i64
  }
  func.func @strict64_multiply_add(%a: i64, %b: i64) -> i64 {
    %x = arith.bitcast %a : i64 to f64
    %y = arith.bitcast %b : i64 to f64
    %p = arith.mulf %x, %y : f64
    %r = arith.addf %p, %x : f64
    %bits = arith.bitcast %r : f64 to i64
    return %bits : i64
  }
  func.func @strict64_sub(%a: i64, %b: i64) -> i64 {
    %x = arith.bitcast %a : i64 to f64
    %y = arith.bitcast %b : i64 to f64
    %r = arith.subf %x, %y : f64
    %bits = arith.bitcast %r : f64 to i64
    return %bits : i64
  }
  func.func @strict64_neg(%a: i64, %b: i64) -> i64 {
    %x = arith.bitcast %a : i64 to f64
    %r = arith.negf %x : f64
    %bits = arith.bitcast %r : f64 to i64
    return %bits : i64
  }
  func.func @strict64_div(%a: i64, %b: i64) -> i64 {
    %x = arith.bitcast %a : i64 to f64
    %y = arith.bitcast %b : i64 to f64
    %r = arith.divf %x, %y : f64
    %bits = arith.bitcast %r : f64 to i64
    return %bits : i64
  }
  func.func @strict64_association(%a: i64, %b: i64, %c: i64) -> i64 {
    %x = arith.bitcast %a : i64 to f64
    %y = arith.bitcast %b : i64 to f64
    %z = arith.bitcast %c : i64 to f64
    %p = arith.addf %x, %y : f64
    %r = arith.addf %p, %z : f64
    %bits = arith.bitcast %r : f64 to i64
    return %bits : i64
  }

  func.func @strict_add(%a: i32, %b: i32) -> i32 {
    %x = arith.bitcast %a : i32 to f32
    %y = arith.bitcast %b : i32 to f32
    %r = arith.addf %x, %y : f32
    %bits = arith.bitcast %r : f32 to i32
    return %bits : i32
  }
  func.func @strict_mul(%a: i32, %b: i32) -> i32 {
    %x = arith.bitcast %a : i32 to f32
    %y = arith.bitcast %b : i32 to f32
    %r = arith.mulf %x, %y : f32
    %bits = arith.bitcast %r : f32 to i32
    return %bits : i32
  }
  func.func @strict_multiply_add(%a: i32, %b: i32) -> i32 {
    %x = arith.bitcast %a : i32 to f32
    %y = arith.bitcast %b : i32 to f32
    %p = arith.mulf %x, %y : f32
    %r = arith.addf %p, %x : f32
    %bits = arith.bitcast %r : f32 to i32
    return %bits : i32
  }
  func.func @strict_sub(%a: i32, %b: i32) -> i32 {
    %x = arith.bitcast %a : i32 to f32
    %y = arith.bitcast %b : i32 to f32
    %r = arith.subf %x, %y : f32
    %bits = arith.bitcast %r : f32 to i32
    return %bits : i32
  }
  func.func @strict_neg(%a: i32, %b: i32) -> i32 {
    %x = arith.bitcast %a : i32 to f32
    %r = arith.negf %x : f32
    %bits = arith.bitcast %r : f32 to i32
    return %bits : i32
  }
  func.func @strict_div(%a: i32, %b: i32) -> i32 {
    %x = arith.bitcast %a : i32 to f32
    %y = arith.bitcast %b : i32 to f32
    %r = arith.divf %x, %y : f32
    %bits = arith.bitcast %r : f32 to i32
    return %bits : i32
  }
}
