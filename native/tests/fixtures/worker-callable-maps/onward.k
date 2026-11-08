fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
fn main()->i64 ! TaskError:
  callbacks=try parallel:
    try await spawn map([40],make)
  return try parallel:
    values=try await spawn map(callbacks,fn(callback):return callback(2))
    values[0]
