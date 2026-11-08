fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
fn main()->i64 ! TaskError:
  callbacks=[make(1),make(40)]
  values=try parallel:
    try await spawn slice(callbacks,1,2)
  return values[0](2)
