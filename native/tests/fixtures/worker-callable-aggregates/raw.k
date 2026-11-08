enum Choice:
  Empty
  Some:
    run:fn(i64)->i64
fn make(offset:i64)->[fn(i64)->i64]:
  values=[offset]
  return [fn(value):return values[0]+value,fn(value):return values[0]-value]
fn makeChoice(offset:i64)->Choice:
  values=[offset]
  return Choice.Some {run:fn(value:i64):return values[0]+value}
fn makeCaptured()->fn(i64)->i64:
  callbacks:[fn(i64)->i64]=[fn(value):return value+40]
  return fn(value:i64):return callbacks[0](value)
fn makeWords()->[fn(String)->i64]:
  return [fn(text):return length(text)]
fn apply(callbacks:[fn(i64)->i64])->i64:
  return callbacks[0](2)
fn main()->i64 ! TaskError:
  extra=makeCaptured()
  extra(2)
  callbacks=try parallel:
    try await spawn make(21)
  return callbacks[0](2)+callbacks[1](2)
