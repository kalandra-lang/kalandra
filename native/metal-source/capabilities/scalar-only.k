fn operation(value:f32)->f32:
  return value*2
fn main()->f32 ! TaskError:
  return try parallel:
    try await spawn operation(2) on devices.gpu()
