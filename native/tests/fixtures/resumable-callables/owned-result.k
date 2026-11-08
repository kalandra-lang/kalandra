fn append(values:[i64])->[i64]:
  return concat(values,[2])
fn run(values:[i64])->[i64] ! TaskError:
  return try parallel:
    try await spawn append(values)
fn main()->i64 ! TaskError:
  callback=run
  values=try callback([20,20])
  return values[0]+values[1]+values[2]
