fn make(offset:i64)->[fn(i64)->i64]:
  values=[offset]
  return [fn(value):return values[0]+value,fn(value):return values[0]-value]
fn main()->i64 ! TaskError:
  callbacks=try parallel:
    work=spawn make(21)
    first=try await work
    second=try await work
    first[0](2)+second[1](2)
    second
  return callbacks[0](2)+callbacks[1](2)
