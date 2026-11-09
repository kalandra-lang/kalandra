fn double(value:i64)->i64:
  return value*2
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn (fn(value:i64):return value*2)(21)
