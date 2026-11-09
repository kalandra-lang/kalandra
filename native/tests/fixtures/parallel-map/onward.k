fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
fn produce()->[fn(i64)->i64] ! TaskError:
  return try parallelMap([40],make)
fn main()->i64 ! TaskError:
  callbacks=try parallel:
    try await spawn produce()
  values=try parallelMap(callbacks,fn(callback):return callback(2))
  return values[0]
