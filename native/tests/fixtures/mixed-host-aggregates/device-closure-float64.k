fn entry(value:f64)->f64:
  twice=fn(x:f64):return x+x
  return twice(value)
fn main()->i64 ! TaskError:
  return try parallel:
    result=try await spawn entry(f64(3)) on devices.gpu()
    i64(result)
