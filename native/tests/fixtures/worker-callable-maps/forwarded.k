fn double(value:i64)->i64:
  return value*2
fn process(values:[i64],callback:fn(i64)->i64)->[i64] ! TaskError:
  return try parallel:
    try await spawn map(values,callback)
fn main()->i64 ! TaskError:
  values=try process([21],double)
  return values[0]
