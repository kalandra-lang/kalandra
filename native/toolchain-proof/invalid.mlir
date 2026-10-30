module {
  func.func @invalid() -> f32 {
    %value = arith.constant 1.0 : f64
    return %value : f64
  }
}
