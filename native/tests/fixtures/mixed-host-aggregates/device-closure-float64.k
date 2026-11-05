fn entry(value:float64)->float64:
  twice=fn(x:float64):x+x
  twice(value)
fn main()->int ! TaskError:
  try parallel:
    result=try await spawn entry(float64(3)) on devices.gpu()
    int(result)
