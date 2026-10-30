module {
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
