fn operation(value:float32)->float32:
  value*2
fn main()->float32 ! TaskError:
  try parallel:
    try await spawn operation(2) on devices.gpu()
