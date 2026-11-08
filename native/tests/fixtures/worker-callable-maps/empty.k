fn double(value:i64)->i64:
  return value*2
fn main()->i64 ! TaskError:
  inputs:[i64]=[]
  f=double
  return try parallel:
    values=try await spawn map(inputs,f)
    length(values)+42
