fn make(offset:i64)->fn(i64)->i64:
  values=[offset]
  return fn(value:i64):return values[0]+value
fn main()->i64 ! TaskError:
  selected=try parallel:
    work=spawn make(40)
    first=try await work
    second=try await work
    first(2)+second(2)
    second
  return selected(2)
