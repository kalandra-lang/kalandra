fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
fn select(callbacks:[fn(i64)->i64],start:i64,end:i64)->[fn(i64)->i64] ! TaskError:
  return try parallel:
    try await spawn slice(callbacks,start,end)
fn main()->i64 ! TaskError:
  selected=try select([make(0),make(40)],1,2)
  return selected[0](2)
