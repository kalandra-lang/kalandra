fn double(value:i64)->i64:
  return value*2
fn main()->i64 ! TaskError:
  return try parallel:
    work=spawn parallelMap([21],double)
    values=try await work
    values[0]
