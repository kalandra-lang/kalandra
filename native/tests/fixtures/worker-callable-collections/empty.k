fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
fn main()->i64 ! TaskError:
  callbacks:[fn(i64)->i64]=[]
  values=try parallel:
    try await spawn concat(callbacks,[make(40)])
  selected=try parallel:
    try await spawn slice(values,0,0)
  count=try parallel:
    try await spawn length(selected)
  return count+values[0](2)
